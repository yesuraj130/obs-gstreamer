#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern const char *obs_gstreamer_version;
extern const char *obs_gstreamer_commit;
extern const char *obs_gstreamer_build_date;
extern const char *obs_gstreamer_repo_url;
extern const char *obs_gstreamer_build_type;

void obs_gstreamer_log_build_info(void);

#ifdef __cplusplus
}
#endif
