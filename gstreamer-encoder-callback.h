/*
 * obs-gstreamer: Direct GPU Texture Encoder Callback Source for Master Pipeline
 */

#ifndef GSTREAMER_ENCODER_CALLBACK_H
#define GSTREAMER_ENCODER_CALLBACK_H

#include <obs.h>
#include <obs-encoder.h>
#include "gstreamer-master-pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

void gstreamer_encoder_callback_register(void);
void gstreamer_encoder_callback_unregister(void);

void gstreamer_encoder_callback_set_master_pipeline(obs_encoder_t *encoder, gst_master_pipeline_t *pipe);

#ifdef __cplusplus
}
#endif

#endif /* GSTREAMER_ENCODER_CALLBACK_H */
