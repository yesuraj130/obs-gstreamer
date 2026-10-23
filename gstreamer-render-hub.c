#include "gstreamer-render-hub.h"
#include <obs-module.h>
#include <gst/gst.h>

//Master GPU Frame Capture Callback hooked into OBS render thread
void hub_render_callback(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	gst_master_pipeline_t *hub = (gst_master_pipeline_t *)param;
	if (!hub || !hub->view) return;

	// Render if ANY RTSP client is connected OR ANY pipeline branch is active
	if (g_atomic_int_get(&hub->connected_client_count) <= 0 && g_atomic_int_get(&hub->pipeline_branch_count) <= 0)
	{
		hub->first_frame_primed = false;
		return;
	}

	uint32_t cur = hub->pingpong_idx;
	uint32_t prev = 1 - cur;

	// 1. Render the offscreen view into the current pingpong texture
	if (hub->texrender[cur])
	{
		gs_texrender_begin(hub->texrender[cur], hub->width, hub->height);
		gs_enable_blending(false);
		obs_view_render(hub->view);
		gs_texrender_end(hub->texrender[cur]);

		// Copy texture into staging surface for zero-stall DMA
		gs_texture_t *tex = gs_texrender_get_texture(hub->texrender[cur]);
		if (tex && hub->stagesurf[cur]) gs_stage_texture(hub->stagesurf[cur], tex);
	}

	// 2. Map the PREVIOUS frame's staging surface (GPU finished DMA, zero CPU wait)
	if (hub->first_frame_primed && hub->stagesurf[prev])
	{
		uint8_t *data = NULL;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(hub->stagesurf[prev], &data, &linesize))
		{
			gst_master_pipeline_push_raw_frame(hub, data, linesize);
			gs_stagesurface_unmap(hub->stagesurf[prev]);
		}
	}
	else
	{
		hub->first_frame_primed = true;
	}

	// Swap pingpong index
	hub->pingpong_idx = prev;
}

void gst_render_hub_init_hook(void)
{
	// Render hook is registered dynamically when master pipeline starts
}
