/*
 * obs-gstreamer: Direct GPU Render Hub for Ultra-Low Latency Multi-Port RTSP
 *
 * Encodes directly from OBS GPU render views and serves multiple RTSP ports
 * without OBS output round-trips.
 */

#ifndef GSTREAMER_RENDER_HUB_H
#define GSTREAMER_RENDER_HUB_H

#include <stdbool.h>
#include <stdint.h>
#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gst_hub_branch gst_hub_branch_t;

typedef struct {
	const char *name;
	const char *source_type;   /* "Program Output", "Scene", "Source" */
	const char *source_name;   /* Name of scene or source */
	const char *rtsp_service;  /* Port, e.g. "8554" */
	const char *rtsp_mount;    /* Mount point, e.g. "/live", "/primary" */
	const char *encoder_type;  /* "nvh264enc", "qsvh264enc", "vaapih264enc", "x264enc" */
	int bitrate_kbps;          /* e.g. 4000 */
	int keyint_sec;            /* e.g. 1 or 2 */
	const char *master_pipeline; /* Custom master pipeline string (or NULL for auto) */
	const char *rtsp_pipeline;   /* Custom RTSP branch pipeline string (or NULL for auto) */
} gst_hub_output_params_t;

/* Start a direct RTSP output branch on the render hub */
gst_hub_branch_t *gst_render_hub_start_branch(const gst_hub_output_params_t *params);

/* Stop an active RTSP output branch */
void gst_render_hub_stop_branch(gst_hub_branch_t *branch);

/* Check if a branch is currently active */
bool gst_render_hub_is_branch_active(const gst_hub_branch_t *branch);

/* Clean shutdown of all render hub resources */
void gst_render_hub_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GSTREAMER_RENDER_HUB_H */
