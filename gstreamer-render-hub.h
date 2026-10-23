/*
 * obs-gstreamer: Direct GPU Render Hub (OBS Render Callback & Frame Capture)
 */

#ifndef GSTREAMER_RENDER_HUB_H
#define GSTREAMER_RENDER_HUB_H

#include "gstreamer-rtspserver.h"
#include "gstreamer-master-pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize or hook render callback if needed */
void gst_render_hub_init_hook(void);

#ifdef __cplusplus
}
#endif

#endif /* GSTREAMER_RENDER_HUB_H */
