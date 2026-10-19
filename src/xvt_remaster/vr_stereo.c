/* VR stereo hand-off between the Quest/PICO host (src/xvt_vr) and the
 * remaster.
 *
 * The host publishes the two eye views before each XvtRemaster_Frame(). While
 * the modern flight scene is shown (not the map, not the hyperspace streak
 * transition) the remaster renders the 3D world once per eye into the host's
 * eye images (flight_scene.c) and the HUD/cockpit alone over transparent
 * black as the flat frame (flight_pipeline.c), which the host shows as a quad
 * matching the game camera's field of view. Everything else stays flat. */

#include "xvt_remaster/vr_stereo.h"

#include "aeron/asset/opt_model.h"
#include "xvt_remaster/flight.h"

#include <math.h>
#include <stddef.h>

static XvtVrStereoFrame g_vr_frame;
static int              g_vr_frame_valid;
static int              g_vr_rendered;
static float            g_vr_tangents[4];
static int              g_vr_tangents_valid;

void XvtRemasterVr_SetStereoFrame(const XvtVrStereoFrame* frame) {
	g_vr_frame_valid = frame != NULL && frame->eyes[0].target && frame->eyes[1].target;
	if (frame)
		g_vr_frame = *frame;
}

const XvtVrStereoFrame* XvtRemasterVr_PublishedFrame(void) { return g_vr_frame_valid ? &g_vr_frame : NULL; }

int XvtRemasterVr_Wanted(const XvtRenderSnapshot* s) {
	return g_vr_frame_valid && s && s->flight_valid && s->camera.valid && s->scene_kind == XVT_SCENE_FLIGHT &&
		   s->presented_target == XVT_TARGET_FLIGHT_MAIN && !s->camera.map_mode &&
		   s->hyperspace.phase != XVT_SNAP_HYPERSPACE_TRANSITION;
}

void XvtRemasterVr_SetRendered(int rendered, const struct XvtPreparedFlight* frame) {
	g_vr_rendered = rendered;
	if (!rendered || !frame)
		return;
	/* Frustum edges of the flat HUD frame (XvtRenderMath_BuildMainView):
	 * ndc_x = x/(z tanH) + off_x, ndc_y = -y/(z tanV) + off_y, y down. */
	const AeronSceneCamera* c = &frame->view.camera;
	const float th = tanf(c->h_half_rad), tv = tanf(c->v_half_rad);
	g_vr_tangents[0] = th * (1.0f + c->proj_x_offset);
	g_vr_tangents[1] = th * (1.0f - c->proj_x_offset);
	g_vr_tangents[2] = tv * (1.0f - c->proj_y_offset);
	g_vr_tangents[3] = tv * (1.0f + c->proj_y_offset);
	g_vr_tangents_valid = 1;
}

int XvtRemasterVr_StereoRendered(void) { return g_vr_rendered; }

int XvtRemasterVr_HudTangents(float* tan_left, float* tan_right, float* tan_up, float* tan_down) {
	if (!g_vr_tangents_valid)
		return 0;
	*tan_left = g_vr_tangents[0];
	*tan_right = g_vr_tangents[1];
	*tan_up = g_vr_tangents[2];
	*tan_down = g_vr_tangents[3];
	return 1;
}

float XvtRemasterVr_WorldUnitsPerMetre(void) { return 1.0f / AERON_OPT_METERS_PER_UNIT; }
