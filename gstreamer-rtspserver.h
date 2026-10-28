/*
 * obs-gstreamer: RTSP Server and Branch Management
 */

#ifndef GSTREAMER_RTSPSERVER_H
#define GSTREAMER_RTSPSERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <obs.h>
#include "gstreamer-master-pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gst_hub_branch gst_hub_branch_t;

typedef struct {
	const char *name;
	const char *source_type;
	const char *source_name;
	const char *rtsp_service;
	const char *rtsp_mount;
	const char *encoder_type;
	int bitrate_kbps;
	int keyint_sec;
	const char *master_pipeline;
	const char *rtsp_pipeline;
	const char *render_mode;
	gst_master_pipeline_t *master_pipe;
} gst_hub_output_params_t;

gst_hub_branch_t *gst_rtsp_branch_start(gst_master_pipeline_t *pipeline,
                                        const char *name,
                                        const char *service,
                                        const char *mount,
                                        const char *encoder_type,
                                        const char *rtsp_pipeline);

gst_hub_branch_t *gst_render_hub_start_branch_data(obs_data_t *settings);
gst_hub_branch_t *gst_render_hub_start_pipeline_branch(const char *name, const char *source_type, const char *source_name, const char *pipeline_str);
gst_hub_branch_t *gst_render_hub_start_branch(const gst_hub_output_params_t *params);
void gst_render_hub_stop_branch(gst_hub_branch_t *branch);
bool gst_render_hub_is_branch_active(const gst_hub_branch_t *branch);
void gst_render_hub_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GSTREAMER_RTSPSERVER_H */
