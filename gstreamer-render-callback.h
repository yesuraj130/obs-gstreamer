/*
 * obs-gstreamer: Direct GPU Render Callback Source for Master Pipeline
 */

#ifndef GSTREAMER_RENDER_CALLBACK_H
#define GSTREAMER_RENDER_CALLBACK_H

#include <obs.h>
#include "gstreamer-helper.h"
#include "gstreamer-master-pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gst_render_callback gst_render_callback_t;

gst_render_callback_t *gst_render_callback_create(gst_source_type_t type,
                                                  const char *scene_name,
                                                  const char *item_name,
                                                  const char *custom_master_pipe,
                                                  const char *encoder_type,
                                                  int bitrate_kbps,
                                                  int keyint_sec,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint32_t fps_num,
                                                  uint32_t fps_den);

gst_master_pipeline_t *gst_render_callback_get_pipeline(gst_render_callback_t *cb);
void gst_render_callback_destroy(gst_render_callback_t *cb);

#ifdef __cplusplus
}
#endif

#endif /* GSTREAMER_RENDER_CALLBACK_H */
