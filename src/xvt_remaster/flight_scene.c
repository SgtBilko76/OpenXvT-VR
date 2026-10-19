#include "aeron/asset/opt_model.h"
#include "xvt_remaster/component_animation.h"
#include "xvt_remaster/config.h"
#include "xvt_remaster/effects.h"
#include "xvt_remaster/engine_glows.h"
#include "xvt_remaster/flight.h"
#include "xvt_remaster/flight_map.h"
#include "xvt_remaster/flight_pipeline.h"
#include "xvt_remaster/hud_renderer.h"
#include "xvt_remaster/lighting.h"
#include "xvt_remaster/ship.h"
#include "xvt_remaster/sky.h"
#include "xvt_remaster/vr_cockpit.h"
#include "xvt_remaster/vr_stereo.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static AeronScene3D* g_scene;
static AeronSceneMeshTable* g_tables;
static int g_width, g_height, g_samples, g_outputValid;
/* VR stereo: the eye being rendered (NULL on the flat path) and its copy of
 * the prepared frame with the eye camera. */
static const XvtVrStereoEye* g_eye;
static XvtPreparedFlight g_eyeFrame;

static int Ensure(int width, int height) {
	int samples = XvtRemasterConfig_Effective()->msaa_samples;
	if (!g_tables)
		g_tables = calloc(XVT_SNAP_OBJECTS * 3, sizeof *g_tables);
	if (!g_tables)
		return 0;
	if (g_scene && g_width == width && g_height == height && samples == g_samples)
		return 1;
	AeronScene3D* scene =
		AeronScene_Create(&(AeronScene3DDesc) { .rt_width = width,
												.rt_height = height,
												.color_format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
												.with_normal_rt = 1,
												.sample_count = (AeronSampleCount)samples,
												.view_space_to_meters = AERON_OPT_METERS_PER_UNIT });
	if (!scene)
		return 0;
	AeronScene_SetClearColor(scene, (const float[4]) { 0, 0, 0, 1 });
	XvtFlightPipeline_ForgetSources();
	AeronScene_Destroy(g_scene);
	g_scene = scene;
	g_width = width;
	g_height = height;
	g_samples = samples;
	g_outputValid = 0;
	return 1;
}

static int Eligible(const XvtRenderSnapshot* s, const XvtSnapObject* o) {
	if (o->slot_class == XVT_SLOT_STATIC &&
		(o->genus < CRAFT_GENUS_MINE || o->genus > CRAFT_GENUS_SMALL_DEBRIS))
		return 0;
	if (o->slot_class != XVT_SLOT_STATIC && o->genus > CRAFT_GENUS_OTHER_PROJECTILE &&
		o->genus != CRAFT_GENUS_SMALL_DEBRIS && o->genus != CRAFT_GENUS_EXPLOSION &&
		o->genus != CRAFT_GENUS_OBSTACLE)
		return 0;
	if (o->slot_class == XVT_SLOT_LOCAL_TRANSIENT &&
		(!s->sky.debris_enabled || s->sky.proving_grounds ||
		 s->camera.hyperspace_phase == XVT_SNAP_HYPERSPACE_TRANSITION))
		return 0;
	return 1;
}

int XvtRemasterFlight_PrepareResources(int width, int height) {
	AeronScene3D* previous = g_scene;
	return Ensure(width, height) &&
		   (previous == g_scene || XvtFlightPipeline_PrepareSceneResources(g_scene, 1));
}

static void DrawHudAfterUpscale(AeronCommandBuffer* cmd, AeronRenderPass* pass, int width, int height,
								void* user) {
	(void)width;
	(void)height;
	XvtHudRenderer_Draw(cmd, pass, AeronScene_SceneRt((AeronScene3D*)user));
}

static void QuatToRows(const float q[4], float m[9]) {
	const float w = q[0], x = q[1], y = q[2], z = q[3];
	m[0] = 1 - 2 * (y * y + z * z);
	m[1] = 2 * (x * y - w * z);
	m[2] = 2 * (x * z + w * y);
	m[3] = 2 * (x * y + w * z);
	m[4] = 1 - 2 * (x * x + z * z);
	m[5] = 2 * (y * z - w * x);
	m[6] = 2 * (x * z - w * y);
	m[7] = 2 * (y * z + w * x);
	m[8] = 1 - 2 * (x * x + y * y);
}

static void RowsToQuat(const float m[9], float q[4]) {
	const float trace = m[0] + m[4] + m[8];
	if (trace > 0) {
		const float sq = sqrtf(trace + 1) * 2;
		q[0] = 0.25f * sq;
		q[1] = (m[7] - m[5]) / sq;
		q[2] = (m[2] - m[6]) / sq;
		q[3] = (m[3] - m[1]) / sq;
	} else if (m[0] > m[4] && m[0] > m[8]) {
		const float sq = sqrtf(1 + m[0] - m[4] - m[8]) * 2;
		q[0] = (m[7] - m[5]) / sq;
		q[1] = 0.25f * sq;
		q[2] = (m[1] + m[3]) / sq;
		q[3] = (m[2] + m[6]) / sq;
	} else if (m[4] > m[8]) {
		const float sq = sqrtf(1 + m[4] - m[0] - m[8]) * 2;
		q[0] = (m[2] - m[6]) / sq;
		q[1] = (m[1] + m[3]) / sq;
		q[2] = 0.25f * sq;
		q[3] = (m[5] + m[7]) / sq;
	} else {
		const float sq = sqrtf(1 + m[8] - m[0] - m[4]) * 2;
		q[0] = (m[3] - m[1]) / sq;
		q[1] = (m[2] + m[6]) / sq;
		q[2] = (m[5] + m[7]) / sq;
		q[3] = 0.25f * sq;
	}
}

/* Replaces the scene camera by one VR eye (as in the X-Wing Alliance port):
 * the head pose relative to the seat is applied on top of the game ("body")
 * camera and the projection becomes the eye's asymmetric frustum. Game eye
 * axes are x right, y down, z forward; OpenXR's are x right, y up, -z
 * forward (C = diag(1,-1,-1)). */
static void ApplyVrEye(XvtRenderView* view, const XvtVrStereoEye* eye) {
	AeronSceneCamera* camera = &view->camera;
	float body[9], head[9], rel[9], rows[9];
	const float q[4] = { eye->orientation[3], eye->orientation[0], eye->orientation[1], eye->orientation[2] };
	QuatToRows(camera->ori, body); /* world -> body eye, rows = body axes */
	QuatToRows(q, head);           /* head-local -> LOCAL, OpenXR axes */
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			rel[r * 3 + c] = head[r * 3 + c] * (r == 0 ? 1.0f : -1.0f) * (c == 0 ? 1.0f : -1.0f);
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			rows[r * 3 + c] =
				rel[0 * 3 + r] * body[0 * 3 + c] + rel[1 * 3 + r] * body[1 * 3 + c] + rel[2 * 3 + r] * body[2 * 3 + c];
	RowsToQuat(rows, camera->ori);
	/* Eye position: seat-relative metres -> body game axes -> world units. */
	const float upm = 1.0f / AERON_OPT_METERS_PER_UNIT;
	const float local[3] = { eye->position_m[0] * upm, -eye->position_m[1] * upm, -eye->position_m[2] * upm };
	for (int axis = 0; axis < 3; ++axis)
		camera->pos[axis] +=
			body[0 * 3 + axis] * local[0] + body[1 * 3 + axis] * local[1] + body[2 * 3 + axis] * local[2];
	const float tl = eye->tan_left, tr = eye->tan_right, tu = eye->tan_up, td = eye->tan_down;
	camera->h_half_rad = atanf((tl + tr) * 0.5f);
	camera->v_half_rad = atanf((tu + td) * 0.5f);
	camera->proj_x_offset = (tl - tr) / (tl + tr);
	camera->proj_y_offset = (td - tu) / (tu + td);
	camera->viewport = (AeronRectI) { 0, 0, eye->target_width, eye->target_height };
	AeronScene_ComputeViewProj(camera, view->view_proj);
}

int XvtRemasterFlight_Render(AeronCommandBuffer* cmd, const XvtRenderSnapshot* s,
							 const XvtRenderSnapshot* p) {
	const XvtPreparedFlight* frame = XvtRemasterFlight_Current();
	if (!frame || !s->flight_valid) {
		g_outputValid = 0;
		return 1;
	}
	if (g_eye) {
		/* One head-tracked eye: no motion history, no HUD. */
		memcpy(&g_eyeFrame, frame, sizeof g_eyeFrame);
		ApplyVrEye(&g_eyeFrame.view, g_eye);
		g_eyeFrame.previous_view = g_eyeFrame.view;
		g_eyeFrame.reset_history = 1;
		g_eyeFrame.regenerate_motion = 0;
		g_eyeFrame.render_needed = 1;
		frame = &g_eyeFrame;
	} else if (s->camera.map_mode) {
		if (!frame->render_needed && g_outputValid)
			return 1;
		g_outputValid = XvtFlightMap_Render(cmd, s, &frame->view);
		return g_outputValid;
	}
	int recreate = !g_scene || g_width != frame->view.camera.viewport.width ||
				   g_height != frame->view.camera.viewport.height ||
				   g_samples != XvtRemasterConfig_Effective()->msaa_samples;
	if (!frame->render_needed && !recreate && g_outputValid)
		return 1;
	if (!Ensure(frame->view.camera.viewport.width, frame->view.camera.viewport.height) ||
		!XvtFlightPipeline_Begin(g_scene, s, frame, frame->reset_history || recreate))
		return 0;
	if (!XvtSky_Prepare(cmd, g_scene, s, &frame->view))
		return 0;
	if (!XvtLighting_Begin(g_scene, s))
		return 0;
	XvtRemasterShip_SetEnvironment(g_scene, &s->lighting, NULL, frame->view.camera.pos);
	int streaks = s->hyperspace.phase == XVT_SNAP_HYPERSPACE_TRANSITION &&
				  s->hyperspace.elapsed_ticks < XVT_SNAP_HYPERSPACE_STREAK_END;
	const XvtRenderSettings* settings = XvtRemasterConfig_Effective();
	int camera_motion = settings->motion_blur.camera_blur || settings->temporal_mode != AERON_TEMPORAL_OFF;
	for (unsigned i = 0; !streaks && i < s->object_count; ++i) {
		const XvtSnapObject* object = &s->objects[i];
		if (!Eligible(s, object))
			continue;
		XvtShipSelection selection;
		if (!XvtRemasterShip_Select(s, object, &selection))
			continue;
		const XvtMeshAsset* asset = XvtRemasterShip_Mesh(s, selection.asset_id);
		if (!asset)
			continue;
		const XvtPreparedObject* pose = &frame->objects[i];
		int hidden_owner =
			object->id.slot == s->camera.focus.slot && !s->camera.external && !s->camera.replay_view;
		AeronSceneMeshTable *table = &g_tables[i * 3], *previous_table = &g_tables[i * 3 + 1];
		float visual[XVT_SNAP_COMPONENTS], prior_visual[XVT_SNAP_COMPONENTS];
		XvtRemasterShip_BuildMeshTable(asset, object, selection.component,
									   XvtComponentAnimation_Angles(s, object, asset, visual), table);
		AeronSceneMeshInstance instance = { .mesh = asset->mesh,
											.variant = object->node_switch,
											.mesh_table = table,
											.prev_mesh_table = table,
											.zero_velocity =
												pose->zero_velocity || recreate || !frame->regenerate_motion,
											.cull_mode = AERON_CULL_BACK };
		memcpy(instance.transform, pose->transform, sizeof instance.transform);
		memcpy(instance.prev_transform, pose->previous_transform, sizeof instance.prev_transform);
		if (!XvtEngineGlows_Submit(cmd, g_scene, s, object, asset, table, instance.transform, !hidden_owner))
			return 0;
		if (hidden_owner)
			continue;
		if (frame->regenerate_motion && !instance.zero_velocity && p && pose->previous_index >= 0 &&
			(unsigned)pose->previous_index < p->object_count) {
			const XvtSnapObject* old = &p->objects[pose->previous_index];
			XvtShipSelection old_selection;
			if (XvtRemasterShip_Select(p, old, &old_selection) &&
				old_selection.asset_id == selection.asset_id) {
				if (s->flight_unlocked || old_selection.component != selection.component ||
					memcmp(old->mesh_rotation, object->mesh_rotation, sizeof object->mesh_rotation) ||
					memcmp(old->component_state, object->component_state, sizeof object->component_state)) {
					XvtRemasterShip_BuildMeshTable(asset, old, old_selection.component,
												   XvtComponentAnimation_Angles(p, old, asset, prior_visual),
												   previous_table);
					instance.prev_mesh_table = previous_table;
				}
			} else
				instance.zero_velocity = 1;
		}
		if (object->genus == CRAFT_GENUS_PLAYER_PROJECTILE || object->genus == CRAFT_GENUS_OTHER_PROJECTILE) {
			XvtEffects_ProjectileMatrix(object, s->camera.world_pos, s->camera.world_pos, instance.transform);
			memcpy(instance.prev_transform, instance.transform, sizeof instance.transform);
			if (!instance.zero_velocity && p && pose->previous_index >= 0)
				XvtEffects_ProjectileMatrix(&p->objects[pose->previous_index],
											camera_motion ? p->camera.world_pos : s->camera.world_pos,
											s->camera.world_pos, instance.prev_transform);
			instance.base_color_emissive_strength =
				XvtRemasterConfig_Effective()->models.opt_projectile_emissive_strength;
			instance.no_local_lights = 1;
			instance.shadow_flags =
				AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW;
			instance.cull_mode = AERON_CULL_NONE;
		}
		int full_model = object->genus != CRAFT_GENUS_OBSTACLE || object->id.slot == s->sky.checkpoint_slot ||
						 object->id.slot == (unsigned)s->sky.checkpoint_slot + 1;
		if (full_model) {
			float radius = XvtRemasterShip_Radius(asset, table, instance.transform);
			if (radius > 0) {
				if (XvtRemasterShip_Visible(&frame->view, instance.transform, radius))
					AeronScene_AddMeshInstance(g_scene, &instance);
				else
					AeronScene_AddShadowCaster(g_scene, &instance);
			}
		}
		if (object->genus == CRAFT_GENUS_OBSTACLE && asset->component_count) {
			unsigned hull = asset->component_count - 1;
			for (unsigned m = 0; m < asset->component_count && m < AERON_MAX_MESH_SLOTS; ++m)
				if (asset->mesh->mesh_rot[m].mesh_type == XVT_SNAP_MESH_HULL) {
					hull = m;
					break;
				}
			AeronSceneMeshTable* hull_table = &g_tables[i * 3 + 2];
			XvtRemasterShip_BuildMeshTable(asset, object, (uint16_t)hull,
										   XvtComponentAnimation_Angles(s, object, asset, visual),
										   hull_table);
			AeronSceneMeshInstance selected = instance;
			selected.mesh_table = hull_table;
			selected.prev_mesh_table = hull_table;
			selected.variant = 1;
			float bound = XvtRemasterShip_Radius(asset, hull_table, selected.transform);
			if (XvtRemasterShip_Visible(&frame->view, selected.transform, bound))
				AeronScene_AddMeshInstance(g_scene, &selected);
			else
				AeronScene_AddShadowCaster(g_scene, &selected);
		}
	}
	if (!streaks)
		XvtEffects_Submit(g_scene, s, p, &s->camera, p ? (camera_motion ? &p->camera : &s->camera) : NULL,
						  frame->regenerate_motion && !frame->reset_history, NULL);
	if (g_eye) {
		/* 3D cockpit at the body camera; the eye offset moves inside it. */
		const XvtPreparedFlight* body = XvtRemasterFlight_Current();
		XvtVrCockpit_Submit(g_scene, s, frame, body->view.camera.pos);
		/* The HUD is drawn once into the overlay frame, not per eye. */
		AeronScene_SetPassHook(g_scene, AERON_SCENE_HOOK_AFTER_UPSCALE, NULL, NULL);
		return 1;
	}
	AeronScene_SetPassHook(g_scene, AERON_SCENE_HOOK_AFTER_UPSCALE, DrawHudAfterUpscale, g_scene);
	if (!XvtFlightPipeline_Finish(cmd, g_scene))
		return 0;
	g_outputValid = 1;
	return 1;
}

int XvtRemasterFlight_RenderEye(AeronCommandBuffer* cmd, const XvtRenderSnapshot* s, const XvtRenderSnapshot* p,
								const XvtVrStereoEye* eye, int eye_index) {
	if (!eye || !eye->target || eye->target_width <= 0 || eye->target_height <= 0 ||
		eye->tan_left + eye->tan_right <= 0 || eye->tan_up + eye->tan_down <= 0)
		return 0;
	g_eye = eye;
	XvtFlightPipeline_SetVrEye(1);
	/* The second eye reuses the first eye's shadow cascades. */
	if (g_scene)
		AeronScene_SetDirectionalShadowReuse(g_scene, eye_index > 0);
	int ok = XvtRemasterFlight_Render(cmd, s, p) && g_scene &&
			 XvtFlightPipeline_ResolveVrEye(cmd, g_scene, eye->target, eye->target_width, eye->target_height);
	if (g_scene)
		AeronScene_SetDirectionalShadowReuse(g_scene, 0);
	XvtFlightPipeline_SetVrEye(0);
	g_eye = NULL;
	/* The flat output now holds stale data until the HUD overlay replaces it. */
	g_outputValid = 0;
	return ok;
}

void XvtRemasterFlight_SetVrOverlayOutput(void) { g_outputValid = 1; }

AeronTexture* XvtRemasterFlight_Output(void) { return g_outputValid ? XvtFlightPipeline_Output() : NULL; }

void XvtRemasterFlight_Shutdown(void) {
	XvtFlightMap_Shutdown();
	XvtSky_Shutdown();
	XvtEngineGlows_Shutdown();
	XvtFlightPipeline_Shutdown();
	AeronScene_Destroy(g_scene);
	g_scene = NULL;
	free(g_tables);
	g_tables = NULL;
	g_outputValid = 0;
	XvtRemasterFlight_Invalidate();
}
