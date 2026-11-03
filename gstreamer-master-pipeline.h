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

#define GST_MASTER_NV12_RING_SLOTS 2

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

typedef struct gst_nv12_color_matrix
{
	float y_coeffs[4];
	float u_coeffs[4];
	float v_coeffs[4];
} gst_nv12_color_matrix_t;

#if HAVE_GST_D3D11
typedef struct gst_d3d11_nv12_slot
{
	ID3D11Texture2D *texture;
	ID3D11RenderTargetView *luma_rtv;
	ID3D11RenderTargetView *chroma_rtv;
	volatile gint in_use;
} gst_d3d11_nv12_slot_t;
#endif

#if HAVE_GST_GL
typedef struct gst_gl_nv12_slot
{
	GLuint tex_y;
	GLuint tex_uv;
	GLuint fbo_y;
	GLuint fbo_uv;
	volatile gint in_use;
	volatile gint plane_refs;
} gst_gl_nv12_slot_t;
#endif

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
	char *selected_encoder;
	GstVideoFormat appsrc_format;
	gst_nv12_color_matrix_t color_matrix;
	const char *colorimetry_str;

#if HAVE_GST_D3D11
	GstD3D11Device *d3d11_device;
	ID3D11VertexShader *d3d11_nv12_vertex_shader;
	ID3D11PixelShader *d3d11_nv12_luma_shader;
	ID3D11PixelShader *d3d11_nv12_chroma_shader;
	ID3D11SamplerState *d3d11_nv12_sampler;
	ID3D11Buffer *d3d11_nv12_cbuffer;
	gst_d3d11_nv12_slot_t d3d11_slots[GST_MASTER_NV12_RING_SLOTS];
	uint32_t d3d11_next_slot;
	bool d3d11_nv12_ready;
	bool d3d11_desc_logged;
#endif
#if HAVE_GST_GL
	GstGLDisplay *gl_display;
	GstGLContext *gl_app_context;
	GstGLContext *gl_context;
	GLuint gl_nv12_vao;
	GLuint gl_nv12_vbo;
	GLuint gl_nv12_vs;
	GLuint gl_nv12_ps_y;
	GLuint gl_nv12_ps_uv;
	GLuint gl_nv12_prog_y;
	GLuint gl_nv12_prog_uv;
	gst_gl_nv12_slot_t gl_slots[GST_MASTER_NV12_RING_SLOTS];
	uint32_t gl_next_slot;
	bool gl_nv12_ready;
	bool gl_tex_logged;
#endif

	gint active_destinations;
	volatile gint pipeline_failed;
	uint64_t texture_push_count;
	uint64_t texture_drop_count;
	uint64_t encoded_sample_count;

	gpointer subscribers_atomic;
	GMutex subscriber_mutex;

	gst_master_state_cb state_cb;
	gpointer state_user_data;
	GMutex state_mutex;
};

bool gst_master_pipeline_element_available(const char *factory_name);
const char *gst_master_pipeline_select_encoder(const char *requested_encoder, bool gpu_input);

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
