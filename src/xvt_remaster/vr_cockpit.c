/* 3D cockpits for VR flight from X-Wing Revival (see vr_cockpit.h). */

#include "xvt_remaster/vr_cockpit.h"

#include "aeron/aeron.h"
#include "aeron/asset/obj_model.h"
#include "aeron/scene/mesh.h"
#include "xvt/assets/object_type.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define VR_COCKPIT_LOG "xvt.cockpit3d"

/* X-Wing Revival ship definitions (Data/Ships/<craft>.def): the cockpit model
 * and the pilot eye inside it (right, up, forward metres at model_scale
 * 0.022; "cockpit_vr" where the def has one). */
typedef struct VrCockpitDef {
	int         model_index; /* XvT ModelIndex (g_modelDefs) */
	const char* file;
	float       eye[3];
} VrCockpitDef;

static const VrCockpitDef k_cockpits[] = {
	{ 0x0, "x-wing_cockpit_hq.obj", { 0.0f, 0.814f, -0.704f } },
	{ 0x1, "y-wing_cockpit_hq.obj", { 0.0f, 0.836f, 4.18f } },
	{ 0x2, "a-wing_cockpit.obj", { 0.0f, 0.56f, -0.4f } },
	{ 0x3, "b-wing_cockpit.obj", { 0.0f, 4.55f, 1.15f } },
	{ 0x4, "tie-fighter_cockpit_vr.obj", { 0.0f, 0.1f, -0.1f } },
	{ 0x5, "tie-interceptor_cockpit_vr.obj", { 0.0f, 0.1f, -0.1f } },
	{ 0x6, "tie-bomber_cockpit_vr.obj", { 1.474f, 0.1f, 1.7152f } },
	{ 0x7, "tie-fighter_cockpit_vr.obj", { 0.0f, 0.1f, -0.1f } },        /* TIE Advanced */
	{ 0x8, "tie-interceptor_cockpit_vr.obj", { 0.0f, 0.1f, -0.1f } },    /* TIE Defender */
	{ 0xF, "gunboat_cockpit.obj", { 0.0f, 0.36f, 0.98f } },              /* Assault Gunboat */
	/* No Revival cockpit: borrow the closest one. */
	{ 0xD, "x-wing_cockpit_hq.obj", { 0.0f, 0.814f, -0.704f } },         /* Z-95 Headhunter */
	{ 0xC, "x-wing_cockpit_hq.obj", { 0.0f, 0.814f, -0.704f } },         /* T-wing */
	{ 0xE, "a-wing_cockpit.obj", { 0.0f, 0.56f, -0.4f } },               /* R-41 Starchaser */
	{ 0xB, "gunboat_cockpit.obj", { 0.0f, 0.36f, 0.98f } },              /* Missile Boat */
};

static struct {
	char                 root[512];
	int                  root_checked;
	const VrCockpitDef*  def;
	AeronSceneMesh*      mesh;
	const VrCockpitDef*  failed[sizeof k_cockpits / sizeof k_cockpits[0]];
	int                  failed_count;
	AeronSceneMeshTable  table;
	int                  active;
	unsigned             logged_missing;
} g_cp;

static int VrCockpit_DirHasModels(const char* dir) {
	/* SDL_IOFromFile also reaches APK assets (relative paths on Android). */
	char path[600];
	snprintf(path, sizeof path, "%s/Models/x-wing_cockpit_hq.obj", dir);
	SDL_IOStream* io = SDL_IOFromFile(path, "rb");
	if (!io)
		return 0;
	SDL_CloseIO(io);
	return 1;
}

/* X-Wing Revival's Models/ and Textures/ cockpit files: a folder on the
 * device first (lets players swap models), then the copy bundled in the APK
 * (assets/XWingRevival, staged by tools/quest/stage_revival_cockpits.py). */
static const char* VrCockpit_Root(void) {
	if (g_cp.root_checked)
		return g_cp.root[0] ? g_cp.root : NULL;
	g_cp.root_checked = 1;
	const char* candidates[] = { SDL_getenv("OPENXVT_REVIVAL_DIR"), "/sdcard/OpenXvT/XWingRevival",
								 "/sdcard/XWingRevival", "XWingRevival" };
	for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
		if (candidates[i] && candidates[i][0] && VrCockpit_DirHasModels(candidates[i])) {
			SDL_strlcpy(g_cp.root, candidates[i], sizeof g_cp.root);
			Aeron_LogInfo(VR_COCKPIT_LOG, "X-Wing Revival cockpits from %s", g_cp.root);
			return g_cp.root;
		}
	}
	Aeron_LogInfo(VR_COCKPIT_LOG, "no X-Wing Revival data found; 3D cockpits off");
	return NULL;
}

static const XvtSnapObject* VrCockpit_Player(const XvtRenderSnapshot* s, unsigned* index) {
	if (!s->flight_valid || s->camera.external || s->camera.replay_view || s->camera.map_mode ||
		s->camera.focus.slot != s->camera.player.slot)
		return NULL;
	for (unsigned i = 0; i < s->object_count; ++i) {
		const XvtSnapObject* o = &s->objects[i];
		if (o->id.slot == s->camera.player.slot && o->id.signature == s->camera.player.signature) {
			*index = i;
			return o->has_craft ? o : NULL;
		}
	}
	return NULL;
}

static const VrCockpitDef* VrCockpit_Find(const XvtSnapObject* player) {
	if (player->object_type >= sizeof g_modelTypeTable / sizeof g_modelTypeTable[0])
		return NULL;
	const int model_index = g_modelTypeTable[player->object_type].modelIndex;
	for (size_t i = 0; i < sizeof k_cockpits / sizeof k_cockpits[0]; ++i)
		if (k_cockpits[i].model_index == model_index)
			return &k_cockpits[i];
	return NULL;
}

static int VrCockpit_Failed(const VrCockpitDef* def) {
	for (int i = 0; i < g_cp.failed_count; ++i)
		if (g_cp.failed[i] == def)
			return 1;
	return 0;
}

static int VrCockpit_Load(AeronCommandBuffer* cmd, const VrCockpitDef* def) {
	const char* root = VrCockpit_Root();
	if (!root)
		return 0;
	char path[700], textures[600];
	snprintf(path, sizeof path, "%s/Models/%s", root, def->file);
	/* Explicit: APK asset paths can't use "Models/../Textures". */
	snprintf(textures, sizeof textures, "%s/Textures", root);
	AeronObjModelBuildOptions options = {
		.scale     = 0.022f,
		.origin    = { def->eye[0], def->eye[1], def->eye[2] },
		.roughness   = 0.75f,
		.texture_dir = textures,
	};
	AeronFlightModel model;
	char             error[256];
	const uint64_t   start = SDL_GetTicksNS();
	if (!Aeron_ObjModelBuildFile(path, &options, &model, error, sizeof error)) {
		Aeron_LogWarn(VR_COCKPIT_LOG, "%s: %s", def->file, error);
		return 0;
	}
	AeronSceneMeshCreateStatus status;
	AeronSceneMesh*            mesh = AeronScene_MeshCreate(cmd, &model, def->file, &status);
	Aeron_FlightModelFree(&model);
	if (!mesh) {
		Aeron_LogWarn(VR_COCKPIT_LOG, "%s: mesh upload failed (%d)", def->file, (int)status);
		return 0;
	}
	if (g_cp.mesh)
		AeronScene_MeshDestroy(g_cp.mesh);
	g_cp.mesh = mesh;
	g_cp.def  = def;
	Aeron_LogInfo(VR_COCKPIT_LOG, "loaded %s in %.0f ms", def->file, (double)(SDL_GetTicksNS() - start) / 1e6);
	return 1;
}

int XvtVrCockpit_Prepare(AeronCommandBuffer* cmd, const XvtRenderSnapshot* s) {
	unsigned             index  = 0;
	const XvtSnapObject* player = s ? VrCockpit_Player(s, &index) : NULL;
	const VrCockpitDef*  def    = player ? VrCockpit_Find(player) : NULL;
	g_cp.active                 = 0;
	if (player && !def && player->object_type != g_cp.logged_missing) {
		g_cp.logged_missing = player->object_type;
		Aeron_LogInfo(VR_COCKPIT_LOG, "no 3D cockpit for craft type %u", (unsigned)player->object_type);
	}
	if (!def || VrCockpit_Failed(def))
		return 0;
	/* Crafts sharing a model and eye point reuse the loaded mesh. */
	if (g_cp.def && g_cp.def != def && strcmp(g_cp.def->file, def->file) == 0 &&
		memcmp(g_cp.def->eye, def->eye, sizeof def->eye) == 0)
		g_cp.def = def;
	if (g_cp.def != def && !VrCockpit_Load(cmd, def)) {
		if (g_cp.failed_count < (int)(sizeof g_cp.failed / sizeof g_cp.failed[0]))
			g_cp.failed[g_cp.failed_count++] = def;
		return 0;
	}
	g_cp.active = 1;
	return 1;
}

void XvtVrCockpit_Submit(AeronScene3D* scene, const XvtRenderSnapshot* s, const XvtPreparedFlight* frame,
						 const float camera_pos[3]) {
	unsigned index = 0;
	if (!g_cp.active || !g_cp.mesh || !VrCockpit_Player(s, &index) || index >= frame->object_count)
		return;
	memset(&g_cp.table, 0, sizeof g_cp.table);
	for (unsigned i = 0; i < AERON_MAX_MESH_SLOTS; ++i) {
		g_cp.table.rows[i][0][0] = g_cp.table.rows[i][1][1] = g_cp.table.rows[i][2][2] = 1.0f;
		g_cp.table.visibility_packed[i >> 2][i & 3] = 1.0f;
		g_cp.table.emissive_packed[i >> 2][i & 3]   = 1.0f;
	}
	/* Fixed to the ship (its rotation and model scale), with the pilot eye
	 * (the model origin) at the game camera. */
	AeronSceneMeshInstance instance = { .mesh            = g_cp.mesh,
										.mesh_table      = &g_cp.table,
										.prev_mesh_table = &g_cp.table,
										.zero_velocity   = 1,
										.cull_mode       = AERON_CULL_NONE };
	memcpy(instance.transform, frame->objects[index].transform, sizeof instance.transform);
	instance.transform[3]  = camera_pos[0];
	instance.transform[7]  = camera_pos[1];
	instance.transform[11] = camera_pos[2];
	memcpy(instance.prev_transform, instance.transform, sizeof instance.transform);
	AeronScene_AddMeshInstance(scene, &instance);
}

int XvtVrCockpit_Active(void) { return g_cp.active && g_cp.mesh; }

void XvtVrCockpit_Shutdown(void) {
	if (g_cp.mesh)
		AeronScene_MeshDestroy(g_cp.mesh);
	memset(&g_cp, 0, sizeof g_cp);
}
