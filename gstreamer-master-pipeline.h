/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Pub-Sub Subscriber Hub
 */

#ifndef GSTREAMER_MASTER_PIPELINE_H
#define GSTREAMER_MASTER_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>
#include <gst/gst.h>
#include <obs.h>

#ifdef _WIN32
#include <gst/d3d11/gstd3d11.h>
#else
#include <gst/gl/gstgl.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	GST_SOURCE_MAIN_OUTPUT = 0,
	GST_SOURCE_SCENE,
	GST_SOURCE_SCENE_ITEM,
} gst_source_type_t;

typedef struct gst_master_pipeline gst_master_pipeline_t;
typedef struct gst_pipeline_subscriber gst_pipeline_subscriber_t;

typedef void (*gst_master_frame_cb)(GstBuffer *buf, bool is_keyframe, gpointer user_data);
typedef void (*gst_master_state_cb)(bool active, gpointer user_data);

struct gst_pipeline_subscriber {
	gst_master_frame_cb cb;
	gpointer user_data;
	char *name;
	bool waiting_for_keyframe;
};

struct gst_master_pipeline {
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint64_t frame_index;

	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;

#ifdef _WIN32
	GstD3D11Device *d3d11_device;
	GstAllocator *d3d11_allocator;
#else
	GstGLDisplay *gl_display;
	GstGLContext *gl_context;
	GstAllocator *gl_allocator;
#endif

	gint active_destinations;

	gpointer subscribers_atomic;
	GMutex subscriber_mutex;

	gst_master_state_cb state_cb;
	gpointer state_user_data;
	GMutex state_mutex;
};

gst_master_pipeline_t *gst_master_pipeline_create(const char *custom_master_pipe,
                                                  const char *encoder_type,
                                                  int bitrate_kbps,
                                                  int keyint_sec,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint32_t fps_num,
                                                  uint32_t fps_den);

/* Compatibility helper */
gst_master_pipeline_t *gst_master_pipeline_get_or_create(const char *source_type,
                                                         const char *source_name,
                                                         const char *custom_master_pipe,
                                                         const char *encoder_type,
                                                         int bitrate_kbps,
                                                         int keyint_sec,
                                                         uint32_t width,
                                                         uint32_t height,
                                                         uint32_t fps_num,
                                                         uint32_t fps_den);

void gst_master_pipeline_destroy(gst_master_pipeline_t *hub);

/* Frame push methods */
void gst_master_pipeline_push_texture(gst_master_pipeline_t *hub, void *gpu_handle, uint64_t pts);
void gst_master_pipeline_push_raw_video(gst_master_pipeline_t *hub, struct video_data *frame);
void gst_master_pipeline_push_raw_frame(gst_master_pipeline_t *hub, const uint8_t *data, uint32_t linesize);

/* Pub-sub distribution */
gst_pipeline_subscriber_t *gst_master_pipeline_subscribe(gst_master_pipeline_t *hub, gst_master_frame_cb cb, gpointer user_data, const char *name);
void gst_master_pipeline_unsubscribe(gst_master_pipeline_t *hub, gst_pipeline_subscriber_t *sub);

/* State and destinations tracking */
void gst_master_pipeline_update_active_destinations(gst_master_pipeline_t *hub, int delta);
void gst_master_pipeline_update_client_count(gst_master_pipeline_t *hub, int delta);
int gst_master_pipeline_get_active_destinations(gst_master_pipeline_t *hub);

void gst_master_pipeline_set_state_listener(gst_master_pipeline_t *hub, gst_master_state_cb cb, gpointer user_data);

/* Shared target resolver helper */
obs_source_t *gst_resolve_target_source(gst_source_type_t type, const char *scene_name, const char *item_name);

#ifdef __cplusplus
}
#endif

#endif /* GSTREAMER_MASTER_PIPELINE_H */
