#include "xvt_remaster/xvt_remaster.h"
#include "xvt_remaster/component_animation.h"

#include "aeron/aeron.h"
#include "aeron/compat/host.h"
#include "xvt_remaster/assets.h"
#include "xvt_remaster/cockpit_loading.h"
#include "xvt_remaster/config.h"
#include "xvt_remaster/flight.h"
#include "xvt_remaster/flight_pipeline.h"
#include "xvt_remaster/frontend.h"
#include "xvt_remaster/hud_renderer.h"
#include "xvt_remaster/preview.h"
#include "xvt_remaster/view_mode.h"
#include "xvt_remaster/vr_cockpit.h"
#include "xvt_remaster/vr_stereo.h"
#include "xvt_runtime/snapshot/render_assets.h"
#include "xvt_runtime/snapshot/render_snapshot.h"

static int g_initialized;
static uint32_t g_lastScene;
static int g_frontendSurfacesReleased;

int XvtRemaster_Init(void) {
	int width;
	int height;
	if (g_initialized)
		return 1;
	if (!Aeron_GetLogicalSize(&width, &height) || width <= 0 || height <= 0) {
		Aeron_LogError("xvt.remaster", "an initialized Aeron host is required");
		return 0;
	}
	if (!XvtRemasterConfig_Sync())
		return 0;
	if (!XvtRemasterAssets_Init()) {
		XvtRemasterAssets_Shutdown();
		XvtRemasterConfig_Shutdown();
		return 0;
	}
	g_lastScene = XVT_SCENE_NONE;
	g_frontendSurfacesReleased = 0;
	XvtRemasterView_Init();
	XvtVrCockpit_SelfTest();
	g_initialized = 1;
	Aeron_LogInfo("xvt.remaster", "driver initialized; modern presentation requested");
	return 1;
}

void XvtRemaster_BeginFrame(const struct AeronInputSnapshot* input) {
	if (!g_initialized)
		return;
	if (!XvtRemasterConfig_Sync()) {
		Aeron_RequestFatalRendererError("rendering configuration update");
		return;
	}
	XvtRemasterView_BeginFrame(input);
}

void XvtRemaster_Frame(int32_t delta_us) {
	const XvtRenderSnapshot* snapshot;
	int assets_ready = 1;
	if (!g_initialized)
		return;
	XvtRemasterVr_SetRendered(0, NULL);
	snapshot = XvtRenderSnapshot_Current();
	if (!snapshot)
		return;
	XvtComponentAnimation_Prepare(snapshot);
	XvtRemasterPreview_BeginFrame();
	if (snapshot->scene_kind != g_lastScene) {
		XvtRemasterFlight_Invalidate();
		g_lastScene = snapshot->scene_kind;
		Aeron_LogDebug("xvt.remaster", "scene %u", g_lastScene);
	}
	int width = 0, height = 0;
	if (!Aeron_GetPresentationPixelSize(&width, &height) || width <= 0 || height <= 0) {
		XvtRemasterFlight_Invalidate();
		return;
	}
	int world_needed = XvtRemasterView_NeedsWorld(snapshot);
	int loading_assets =
		snapshot->cockpit_resources.valid &&
		!XvtHudAssets_HasResources(snapshot->cockpit_resources.definition.resource_generation);
	int prepare_assets = world_needed || loading_assets;
	int frontend_replay = XvtFrontend_NeedsReplay(snapshot, width, height);
	if ((prepare_assets || frontend_replay) &&
		(XvtRemasterAssets_ImagesNeedSync(snapshot) ||
		 (frontend_replay && XvtFrontend_AssetsNeedPreparation(snapshot)) ||
		 (prepare_assets &&
		  (XvtRemasterShip_AssetsNeedSync(snapshot) || XvtRemasterAssets_TexturesNeedSync(snapshot))))) {
		XvtHudRenderer_Invalidate();
		do {
			AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
			if (!cmd) {
				Aeron_RequestFatalRendererError("asset upload command buffer");
				return;
			}
			XvtAssetSyncResult result = XVT_ASSET_SYNC_FAILED;
			if (XvtRemasterAssets_SyncImages(cmd, snapshot)) {
				result = prepare_assets ? XvtRemasterShip_SyncAssets(cmd, snapshot, 64u * 1024u * 1024u, 4096)
										: XVT_ASSET_SYNC_COMPLETE;
				if (prepare_assets && result == XVT_ASSET_SYNC_COMPLETE &&
					!XvtRemasterAssets_SyncTextures(cmd, snapshot))
					result = XVT_ASSET_SYNC_FAILED;
				if (frontend_replay && result != XVT_ASSET_SYNC_FAILED &&
					!XvtFrontend_PrepareAssets(cmd, snapshot))
					result = XVT_ASSET_SYNC_FAILED;
			}
			if (result == XVT_ASSET_SYNC_FAILED) {
				Aeron_CancelCommandBuffer(cmd);
				XvtRemasterAssets_Abort();
				Aeron_RequestFatalRendererError("original asset synchronization");
				return;
			}
			if (!Aeron_SubmitCommandBuffer(cmd)) {
				XvtRemasterAssets_Abort();
				Aeron_RequestFatalRendererError("asset upload submission");
				return;
			}
			XvtRemasterAssets_CommitImages();
			XvtRemasterShip_CommitSyncBatch();
			XvtRemasterAssets_CommitTextures();
			assets_ready = result == XVT_ASSET_SYNC_COMPLETE;
			/* Frontend previews must be available before their once-only ordered replay.
			 * Submit the existing bounded upload batches before consuming that stream. */
		} while (!assets_ready && snapshot->preview_count);
	}
	int resources_ready = assets_ready;
	assets_ready = assets_ready && world_needed;
	if (assets_ready && !XvtRemasterFlight_Prepare(snapshot, XvtRenderSnapshot_Previous(), width, height)) {
		Aeron_RequestFatalRendererError("flight view preparation");
		return;
	}
	const XvtPreparedFlight* frame = assets_ready ? XvtRemasterFlight_Current() : NULL;
	if (frame) {
		width = frame->view.camera.viewport.width;
		height = frame->view.camera.viewport.height;
	}
	if (resources_ready && (prepare_assets || !snapshot->cockpit_resources.valid) &&
		!XvtCockpitLoading_Prepare(snapshot, width, height)) {
		Aeron_RequestFatalRendererError("cockpit resource preparation during loading");
		return;
	}
	if (!assets_ready)
		XvtRemasterFlight_Invalidate();
	XvtFlightPipeline_SetDirect(0, width, height);
	/* VR: head tracking needs a fresh pair of eyes every host frame. */
	const XvtVrStereoFrame* vr = XvtRemasterVr_PublishedFrame();
	int stereo = frame && vr && XvtRemasterVr_Wanted(snapshot) && XvtRemasterView_ModernRequested();
	int direct = !vr && assets_ready && XvtRemasterView_Direct(snapshot, width, height);
	int standalone = world_needed && !snapshot->flight_valid && snapshot->cockpit.valid &&
					 snapshot->presented_target == XVT_TARGET_FLIGHT_MAIN;
	int crt_dirty = frame && XvtRemasterPreview_CrtNeedsRender(snapshot, width, height);
	int crt_visible = frame && snapshot->cockpit.crt.valid && (crt_dirty || XvtRemasterPreview_CrtLinear());
	int hud_dirty = (frame || standalone) &&
					XvtHudRenderer_NeedsPreparation(&snapshot->cockpit, snapshot->world_generation,
													frame ? snapshot->target_boxes : NULL,
													frame ? snapshot->target_box_count : 0,
													frame ? &frame->view : NULL, crt_visible, width, height);
	if (frame && (hud_dirty || crt_dirty))
		XvtRemasterFlight_RequestComposition();
	int render_flight = frame && (stereo || frame->render_needed || !XvtRemasterFlight_Output());
	if (render_flight || (standalone && hud_dirty) || frontend_replay || XvtFlightPipeline_NeedsRetain()) {
		AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
		if (!cmd) {
			Aeron_RequestFatalRendererError("presentation command buffer");
			return;
		}
		int ok = 1;
		/* Before the HUD is built: an active 3D cockpit hides the 2D artwork. */
		XvtVrCockpit_Prepare(cmd, stereo ? snapshot : NULL);
		if (render_flight)
			ok = XvtRemasterPreview_RenderCrt(cmd, snapshot, width, height);
		if (ok && (render_flight || (standalone && hud_dirty)))
			ok = XvtHudRenderer_Prepare(cmd, &snapshot->cockpit, snapshot->world_generation,
										frame ? snapshot->target_boxes : NULL,
										frame ? snapshot->target_box_count : 0, frame ? &frame->view : NULL,
										frame ? XvtRemasterPreview_CrtLinear() : NULL, width, height);
		if (ok && stereo) {
			for (int eye = 0; ok && eye < 2; ++eye)
				ok = XvtRemasterFlight_RenderEye(cmd, snapshot, XvtRenderSnapshot_Previous(), &vr->eyes[eye], eye);
			if (ok)
				ok = XvtFlightPipeline_DrawVrHud(cmd, width, height);
			if (ok)
				XvtRemasterFlight_SetVrOverlayOutput();
		} else if (ok && render_flight)
			ok = XvtRemasterFlight_Render(cmd, snapshot, XvtRenderSnapshot_Previous());
		if (ok && standalone && hud_dirty)
			ok = XvtFlightPipeline_DrawStandaloneHud(cmd, width, height);
		if (ok && !direct)
			ok = XvtFlightPipeline_Retain(cmd);
		if (ok && frontend_replay)
			ok = XvtRemasterPreview_Render(cmd, snapshot, width, height) &&
				 XvtFrontend_Replay(cmd, snapshot, width, height);
		if (!ok) {
			Aeron_CancelCommandBuffer(cmd);
			XvtHudRenderer_Invalidate();
			XvtRemasterPreview_InvalidateCrt();
			Aeron_RequestFatalRendererError("presentation recording");
			return;
		}
		if (!Aeron_SubmitCommandBuffer(cmd)) {
			XvtHudRenderer_Invalidate();
			XvtRemasterPreview_InvalidateCrt();
			Aeron_RequestFatalRendererError("presentation submission");
			return;
		}
		XvtRemasterAssets_CommitImages();
		XvtRemasterVr_SetRendered(stereo, frame);
	}
	/* Finish the last ordered frontend replay before retiring its GPU sources. */
	if (snapshot->frontend_surfaces_released && !g_frontendSurfacesReleased) {
		XvtFrontend_ReleaseForFlight();
		XvtRemasterPreview_ReleaseFrontend();
	}
	g_frontendSurfacesReleased = snapshot->frontend_surfaces_released;
	XvtRenderAssets_Consumed(snapshot->tick_index);
	XvtRemasterView_Present(snapshot, delta_us,
							assets_ready && snapshot->cockpit.valid && XvtRemasterFlight_Output() != NULL,
							direct);
}

void XvtRemaster_Shutdown(void) {
	XvtVrCockpit_Shutdown();
	XvtComponentAnimation_Reset();
	XvtRemasterView_Shutdown();
	XvtFrontend_Shutdown();
	XvtRemasterPreview_Shutdown();
	XvtRemasterFlight_Shutdown();

	XvtHudRenderer_Shutdown();
	XvtCockpitLoading_Reset();
	XvtRemasterAssets_Shutdown();
	XvtRemasterConfig_Shutdown();
	if (!g_initialized)
		return;
	AeronDx5_SetClassicFlightRenderingSuppressed(0);
	g_initialized = 0;
	g_lastScene = XVT_SCENE_NONE;
	Aeron_LogInfo("xvt.remaster", "driver shut down");
}

struct AeronTexture* XvtRemaster_Output(void) {
	const XvtRenderSnapshot* s = XvtRenderSnapshot_Current();
	if (!s || s->scene_kind == XVT_SCENE_MOVIE)
		return NULL;
	/* The successful presentation survives task teardown and invalid cameras. */
	if (s->presented_target != XVT_TARGET_FLIGHT_MAIN)
		return XvtFrontend_Output();
	return XvtFlightPipeline_Output();
}

struct AeronTexture* XvtRemaster_MovieOverlay(void) {
	const XvtRenderSnapshot* s = XvtRenderSnapshot_Current();
	return s && s->scene_kind == XVT_SCENE_MOVIE ? XvtFrontend_MovieOverlay() : NULL;
}
