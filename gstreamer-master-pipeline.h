/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Pub-Sub Subscriber Hub
 */

#ifndef GSTREAMER_MASTER_PIPELINE_H
#define GSTREAMER_MASTER_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <obs.h>

#ifndef HAVE_GST_D3D11
#define HAVE_GST_D3D11 0
#endif

#ifndef HAVE_GST_GL
#define HAVE_GST_GL 0
#endif

#if HAVE_GST_D3D11
#include <d3d11.h>
#include <d3d10_1.h>
#include <gst/d3d11/gstd3d11device.h>
#endif

#if HAVE_GST_GL
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
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

#if HAVE_GST_D3D11
typedef struct gst_d3d11_texture_slot
{
	ID3D11Texture2D *texture;
	ID3D11Texture2D *gst_texture;
	HANDLE shared_handle;
	volatile gint in_use;
} gst_d3d11_texture_slot_t;
#endif

#if HAVE_GST_GL
typedef struct gst_gl_texture_slot
{
	GLuint texture;
	GLuint read_fbo;
	GLuint draw_fbo;
	volatile gint in_use;
} gst_gl_texture_slot_t;
#endif

struct gst_master_pipeline
{
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t frame_index;

	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
	gs_stagesurf_t *stagesurf;
	bool gpu_input;
	char *selected_encoder;
	char *video_codec;
	GstVideoFormat appsrc_format;
	gchar *colorimetry_str;

#if HAVE_GST_D3D11
	GstD3D11Device *d3d11_device;
	ID3D11Device *d3d11_native_device;
	gst_d3d11_texture_slot_t d3d11_slot;
	bool d3d11_texture_ready;
#endif
#if HAVE_GST_GL
	GstGLDisplay *gl_display;
	GstGLContext *gl_app_context;
	GstGLContext *gl_context;
	gst_gl_texture_slot_t gl_slot;
	bool gl_texture_ready;
	bool gl_rgb_pipeline;
#endif
	bool custom_d3d11_memory;

	gint active_destinations;
	volatile gint pipeline_failed;
	uint64_t texture_push_count;
	uint64_t texture_drop_count;
	uint64_t encoded_sample_count;
	gpointer cpu_frame_slot;
	uint64_t cpu_frame_drop_count;

	GPtrArray *subscribers;
	GMutex subscriber_mutex;

	gst_master_state_cb state_cb;
	gpointer state_user_data;
	GMutex state_mutex;
};

bool gst_master_pipeline_element_available(const char *factory_name);
const char *gst_master_pipeline_select_encoder(const char *requested_encoder, const char *video_codec, bool gpu_input);

gst_master_pipeline_t *gst_master_pipeline_create(const char *custom_master_pipe, const char *encoder_type, const char *video_codec, int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, bool gpu_input, gst_master_state_cb state_cb, gpointer user_data);
const char *gst_master_pipeline_get_codec(gst_master_pipeline_t *hub);
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
