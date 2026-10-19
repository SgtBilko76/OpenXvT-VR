#ifndef XVT_REMASTER_FLIGHT_H
#define XVT_REMASTER_FLIGHT_H
#include "xvt_remaster/render_math.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XvtPreparedObject {
	float transform[16], previous_transform[16];
	int32_t previous_index;
	uint8_t zero_velocity;
} XvtPreparedObject;

typedef struct XvtPreparedFlight {
	XvtRenderView view, previous_view;
	XvtLayoutTransform cockpit_layout, frontend_layout;
	AeronRectI content_rect;
	XvtPreparedObject objects[XVT_SNAP_OBJECTS];
	uint32_t object_count;
	uint64_t tick_index, flight_frame_serial;
	float delta_seconds;
	/* Host span between changed poses, matching XWA's held-velocity timing. */
	uint64_t velocity_span_us;
	int valid, reset_history, regenerate_motion, render_needed;
} XvtPreparedFlight;

/* Owns all matrices across snapshot rotation. When regenerate_motion is false,
 * retain the prior velocity result instead of resubmitting a stale previous index. */
int XvtRemasterFlight_Prepare(const XvtRenderSnapshot* current, const XvtRenderSnapshot* previous, int width,
							  int height);
const XvtPreparedFlight* XvtRemasterFlight_Current(void);
void XvtRemasterFlight_Invalidate(void);
void XvtRemasterFlight_RequestComposition(void);
int XvtRemasterFlight_Render(AeronCommandBuffer* cmd, const XvtRenderSnapshot* current,
							 const XvtRenderSnapshot* previous);
/* Borrowed tonemapped SDR/HDR presentation output for the composition driver. */
AeronTexture* XvtRemasterFlight_Output(void);
void XvtRemasterFlight_Shutdown(void);
int XvtRemasterFlight_PrepareResources(int width, int height);
/* VR stereo (vr_stereo.h): renders the current frame for one head-tracked eye
 * (no HUD) straight into the eye's host image. */
struct XvtVrStereoEye;
int XvtRemasterFlight_RenderEye(AeronCommandBuffer* cmd, const XvtRenderSnapshot* current,
								const XvtRenderSnapshot* previous, const struct XvtVrStereoEye* eye, int eye_index);
/* Marks the HUD-only overlay drawn by XvtFlightPipeline_DrawVrHud as the output. */
void XvtRemasterFlight_SetVrOverlayOutput(void);
#ifdef __cplusplus
}
#endif
#endif
