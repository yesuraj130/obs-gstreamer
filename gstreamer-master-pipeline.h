/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Pub-Sub Subscriber Hub
 */

#ifndef GSTREAMER_MASTER_PIPELINE_H
#define GSTREAMER_MASTER_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>
#include <gst/gst.h>
#include <obs.h>

#ifndef HAVE_GST_D3D11
#define HAVE_GST_D3D11 0
#endif

#if HAVE_GST_D3D11
#include <gst/d3d11/gstd3d11device.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif


typedef struct gst_master_pipeline gst_master_pipeline_t;
typedef struct gst_pipeline_subscriber gst_pipeline_subscriber_t;

typedef void (*gst_master_frame_cb)(GstBuffer *buf, bool is_keyframe, gpointer user_data);
typedef void (*gst_master_state_cb)(bool active, gpointer user_data);

struct gst_pipeline_subscriber
{
	gst_master_frame_cb cb;
	gpointer user_data;
	char *name;
	bool waiting_for_keyframe;
};

struct gst_master_pipeline
{
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint64_t frame_index;

	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
	gs_stagesurf_t *stagesurf;
	bool gpu_input;
#if HAVE_GST_D3D11
	GstD3D11Device *d3d11_device;
#endif

	gint active_destinations;
	uint64_t texture_push_count;
	uint64_t encoded_sample_count;

	gpointer subscribers_atomic;
	GMutex subscriber_mutex;

	gst_master_state_cb state_cb;
	gpointer state_user_data;
	GMutex state_mutex;
};

gst_master_pipeline_t *gst_master_pipeline_create(const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, bool gpu_input, gst_master_state_cb state_cb, gpointer user_data);
void gst_master_pipeline_destroy(gst_master_pipeline_t *hub);

/* Frame push methods */
void gst_master_pipeline_push_texture(gst_master_pipeline_t *hub, void *gpu_handle, uint64_t pts);
void gst_master_pipeline_push_raw_video(gst_master_pipeline_t *hub, struct video_data *frame);

/* Pub-sub distribution */
gst_pipeline_subscriber_t *gst_master_pipeline_subscribe(gst_master_pipeline_t *hub, gst_master_frame_cb cb, gpointer user_data, const char *name);
void gst_master_pipeline_unsubscribe(gst_master_pipeline_t *hub, gst_pipeline_subscriber_t *sub);
void gst_master_pipeline_request_keyframe(gst_master_pipeline_t *hub);



#ifdef __cplusplus
}
#endif

#endif /* GSTREAMER_MASTER_PIPELINE_H */
