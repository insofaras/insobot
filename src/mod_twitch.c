#include "module.h"
#include <curl/curl.h>
#include "stb_sb.h"
#include <time.h>
#include <string.h>
#include <yajl/yajl_tree.h>
#include "utils.h"

static bool twitch_init (const IRCCoreCtx*);
static void twitch_cmd  (const char*, const char*, const char*, int);
static void twitch_tick (void);
static bool twitch_save (FILE*);

enum { FOLLOW_NOTIFY };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "twitch",
	.desc     = "Functionality specific to twitch.tv",
	.on_init  = twitch_init,
	.on_cmd   = &twitch_cmd,
	.on_tick  = &twitch_tick,
	.on_save  = &twitch_save,
	.commands = DEFINE_CMDS (
		[FOLLOW_NOTIFY] = "\\fnotify"
	)
};

static const IRCCoreCtx* ctx;
static char** enabled_chans;
static time_t* chan_last_time;
static time_t last_check;

static bool twitch_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	time_t now = time(0);
	last_check = now;

	FILE* f = fopen(ctx->get_datafile(), "r");
	char chan[256];
	while(fscanf(f, "%255s", chan) == 1){
		sb_push(enabled_chans, strdup(chan));
		sb_push(chan_last_time, now);
	}
	fclose(f);

	return true;
}

static void mod_cb(intptr_t result, intptr_t arg){
	if(result) *(bool*)arg = true;
}

static void twitch_cmd(const char* chan, const char* name, const char* arg, int cmd){
	bool has_perms = strcasecmp(chan + 1, name) == 0;
	if(!has_perms) MOD_MSG(ctx, "check_admin", name, &mod_cb, &has_perms);
	if(!has_perms) return;

	switch(cmd){
		case FOLLOW_NOTIFY: {
			if(!*arg++) break;

			bool found = false;
			char** c;
			for(c = enabled_chans; c < sb_end(enabled_chans); ++c){
				if(strcasecmp(*c, chan) == 0){
					found = true;
					break;
				}
			}

			if(strcasecmp(arg, "off") == 0){
				if(found){
					ctx->send_msg(chan, "%s: Disabled follow notifier.", name);
					sb_erase(enabled_chans, c - enabled_chans);
					sb_erase(chan_last_time, c - enabled_chans);
				} else {
					ctx->send_msg(chan, "%s: It's already disabled.", name);
				}
			} else if(strcasecmp(arg, "on") == 0){
				if(found){
					ctx->send_msg(chan, "%s: It's already enabled.", name);
				} else {
					ctx->send_msg(chan, "%s: Enabled follow notifier.", name);
					sb_push(enabled_chans, strdup(chan));
					sb_push(chan_last_time, time(0));
				}
			}
		} break;
	}
}

static size_t curl_callback(char* ptr, size_t sz, size_t nmemb, void* data){
	char** out = (char**)data;
	const size_t total = sz * nmemb;

	memcpy(sb_add(*out, total), ptr, total);

	return total;
}

static const size_t follower_check_interval = 120;
static const char twitch_api_template[] = "https://api.twitch.tv/kraken/channels/%s/follows?limit=10";
static const char* follows_path[] = { "follows", NULL };
static const char* date_path[] = { "created_at", NULL };
static const char* name_path[] = { "user", "display_name", NULL };

static void twitch_check_followers(void){

	char* data = NULL;
	yajl_val root = NULL;

	CURL* curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);

	for(char** chan = enabled_chans; chan < sb_end(enabled_chans); ++chan){

		char* url;
		asprintf(&url, twitch_api_template, *chan + 1);

		curl_easy_setopt(curl, CURLOPT_URL, url);
		CURLcode ret = curl_easy_perform(curl);

		free(url);

		sb_push(data, 0);
		
		if(ret != 0){
			fprintf(stderr, "mod_twitch: curl error %s\n", curl_easy_strerror(ret));
			goto out;
		}

		root = yajl_tree_parse(data, NULL, 0);

		if(!YAJL_IS_OBJECT(root)){
			fprintf(stderr, "mod_twitch: root not object!\n");
			goto out;
		}

		yajl_val follows = yajl_tree_get(root, follows_path, yajl_t_array);
		if(!follows){
			fprintf(stderr, "mod_twitch: follows not array!\n");
			goto out;
		}

		char msg_buf[256] = {};
		size_t new_follow_count = 0;

		for(size_t i = 0; i < follows->u.array.len; ++i){
			yajl_val user = follows->u.array.values[i];

			yajl_val date = yajl_tree_get(user, date_path, yajl_t_string);
			if(!date){
				fprintf(stderr, "mod_twitch date object null!\n");
				goto out;
			}

			yajl_val name = yajl_tree_get(user, name_path, yajl_t_string);
			if(!name){
				fprintf(stderr, "mod_twitch name object null!\n");
				goto out;
			}

			struct tm follow_tm = {};
			char* end = strptime(date->u.string, "%Y-%m-%dT%TZ", &follow_tm);
			if(!end || *end){
				fprintf(stderr, "mod_twitch wrong date format?!\n");
				goto out;
			}

			time_t follow_time = mktime(&follow_tm);

			int j = chan - enabled_chans;
			if(follow_time > chan_last_time[j]){
				++new_follow_count;
				if(i){
					inso_strcat(msg_buf, sizeof(msg_buf), ", ");
				}
				inso_strcat(msg_buf, sizeof(msg_buf), name->u.string);
				chan_last_time[j] = follow_time;
			}
		}

		if(new_follow_count == 1){
			ctx->send_msg(*chan, "Thank you to %s for following the channel! <3", msg_buf);
		} else if(new_follow_count > 1){
			ctx->send_msg(*chan, "Thank you new followers: %s! <3", msg_buf);
		}

		sb_free(data);
		data = NULL;

		yajl_tree_free(root);
		root = NULL;
	}

out:
	if(data) sb_free(data);
	if(root) yajl_tree_free(root);

	curl_easy_cleanup(curl);
}

static void twitch_tick(void){
	time_t now = time(0);

	if(now - last_check > follower_check_interval){
		puts("Checking twitch followers...");
		twitch_check_followers();
		last_check = now;
	}
}

static bool twitch_save(FILE* f){
	for(char** c = enabled_chans; c < sb_end(enabled_chans); ++c){
		fprintf(f, "%s\n", *c);
	}
	return true;
}