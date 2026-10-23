/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Subscriber Hub
 */

#ifndef GSTREAMER_MASTER_PIPELINE_H
#define GSTREAMER_MASTER_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>
#include <gst/gst.h>
#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gst_master_pipeline gst_master_pipeline_t;
typedef struct gst_subscriber_branch gst_subscriber_branch_t;

typedef void (*gst_subscriber_packet_cb)(gst_subscriber_branch_t *branch, GstBuffer *buf, bool is_keyframe, gpointer user_data);

struct gst_subscriber_branch {
	char *name;
	char *service;
	char *mount_point;
	bool active;
	bool is_pipeline;
	bool waiting_for_keyframe;
	int client_count;
	GstElement *appsrc;
	GstElement *pipe;
	GMutex lock;
	gst_master_pipeline_t *pipeline;
	gst_subscriber_branch_t *next;
};

struct gst_master_pipeline {
	char *source_type;
	char *source_name;
	obs_view_t *view;
	obs_source_t *source;

	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint64_t frame_index;

	// Double-buffered GPU ping-pong render targets
	gs_texrender_t *texrender[2];
	gs_stagesurf_t *stagesurf[2];
	uint32_t pingpong_idx;
	bool first_frame_primed;

	// Master GStreamer Hardware Encoder
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;

	// Dedicated GLib RTSP Server Event Loop Thread
	GMainContext *rtsp_context;
	GMainLoop *rtsp_loop;
	GThread *rtsp_thread;

	gint connected_client_count;
	gint pipeline_branch_count;
	GMutex branch_lock;
	gst_subscriber_branch_t *branches;
	size_t active_branch_count;
	bool render_hook_active;
};

/* Global master pipeline instance */
gst_master_pipeline_t *gst_master_pipeline_get_or_create(const char *source_type, const char *source_name,
                                                         const char *custom_master_pipe, const char *encoder_type,
                                                         int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den);

void gst_master_pipeline_destroy(gst_master_pipeline_t *pipeline);

void gst_master_pipeline_push_raw_frame(gst_master_pipeline_t *pipeline, const uint8_t *data, uint32_t linesize);

gst_subscriber_branch_t *gst_master_pipeline_add_branch(gst_master_pipeline_t *pipeline, const char *name,
                                                        const char *service, const char *mount_point,
                                                        const char *launch_str, bool is_pipeline);

void gst_master_pipeline_remove_branch(gst_master_pipeline_t *pipeline, gst_subscriber_branch_t *branch);

void gst_master_pipeline_shutdown_all(void);

#ifdef __cplusplus
}
#endif

#endif /* GSTREAMER_MASTER_PIPELINE_H */
