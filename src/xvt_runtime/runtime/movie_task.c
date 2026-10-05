#include "xvt_runtime/runtime/movie_task.h"

#include "xvt_runtime/runtime/presentation.h"

#include "xvt_runtime/runtime/movie_sync.h"
#include "xvt_runtime/snapshot/render_frontend.h"

#include "aeron/aeron.h"
#include "aeron/video.h"
#include "xvt/frontend/config.h"
#include "xvt/frontend/frontend_display.h"
#include "xvt/frontend/frontend_draw.h"
#include "xvt/frontend/frontend_mission.h"
#include "xvt/frontend/frontend_state.h"
#include "xvt/frontend/frontend_text.h"
#include "xvt/frontend/movie.h"
#include "xvt/input/keyboard.h"
#include "xvt_runtime/storage/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XvtMovieTask {
	AeronVideoPlayer* player;
	char name[256];
	char path[XVT_PATH_CAPACITY];
	char lines[3][256];
	uint16_t* overlay;
	uint64_t generation;
	unsigned int next_cue;
	int active;
	int complete;
	int result;
	int paused;
	int saved_mode;
	int saved_restore;
	int saved_lock;
	int synchronize;
} XvtMovieTask;

static XvtMovieTask g_movie;
static AeronRenderSubmission g_subtitleSubmission;

void XvtMovieTask_SuppressClassicSubtitles(void) {
	Aeron_CancelRenderSubmission(g_subtitleSubmission);
	g_subtitleSubmission = 0;
}

static void XvtMovieTask_Close(void) {
	if (g_movie.player)
		Aeron_VideoClose(g_movie.player);
	g_movie.player = NULL;
	if (g_movieSubtitleFile)
		File_Close(g_movieSubtitleFile);
	g_movieSubtitleFile = NULL;
	free(g_movie.overlay);
	g_movie.overlay = NULL;
}

void XvtMovieTask_ReapFinished(void) {
	if (g_movie.active || !g_movie.player)
		return;
	/* Called at the next host frame, after the final decoder/overlay submission. */
	XvtMovieTask_Close();
	FrontendDisplay_SetWndProcMode(g_movie.saved_mode);
	g_frontState.offscreenRestoreEnabled = g_movie.saved_restore;
	if (g_movie.saved_lock)
		g_drawSurfacePtr = FrontendDisplay_LockBackBuffer();
	Keyboard_FlushCharBuffer();
	g_frontState.mouseClickLatch = 0;
	g_frontState.mouseRightClickLatch = 0;
}

int XvtMovieTask_Begin(const char* name, int synchronize) {
	AeronVideoOpenDesc desc = { 0 };
	char relative[XVT_PATH_CAPACITY];
	int found;
	if (!name || !*name || g_movie.active || g_movie.complete || g_movie.player)
		return 2;
	if (snprintf(g_movie.name, sizeof(g_movie.name), "%s", name) >= (int)sizeof(g_movie.name))
		return 2;
	snprintf(relative, sizeof(relative), "movies/%s.smk", g_movie.name);
	found = XvtStorage_ResolveAsset(relative, g_movie.path, sizeof(g_movie.path));
	if (found != 1 && synchronize && g_frontendMissionSessionMode != FRONTEND_MISSION_SESSION_SINGLEPLAYER) {
		strcpy(g_movie.name, "Flyby1a");
		found = XvtStorage_ResolveAsset("movies/Flyby1a.smk", g_movie.path, sizeof(g_movie.path));
	}
	if (found != 1) {
		Aeron_LogWarn("xvt.movie", "Cannot open movie '%s'", name);
		return 2;
	}
	g_movie.overlay = calloc(640 * 480, sizeof(uint16_t));
	if (!g_movie.overlay)
		return 2;
	desc.vfs = XvtStorage_Vfs();
	desc.root = AERON_VFS_ROOT_ASSET;
	desc.path = g_movie.path;
	desc.autoplay = 1;
	desc.gain = g_gameConfig.sfxDatapadEnabled ? (float)g_gameConfig.sfxDatapadVolume / 10.0f : 0;
	g_movie.player = Aeron_VideoOpen(&desc);
	if (!g_movie.player) {
		XvtMovieTask_Close();
		return 2;
	}
	snprintf(relative, sizeof(relative), "movies/%s.txt", g_movie.name);
	g_movieSubtitleFile = File_Open(relative, "r");
	memset(g_movie.lines, 0, sizeof(g_movie.lines));
	g_movie.next_cue = 0;
	g_movie.saved_mode = FrontendDisplay_GetWndProcMode();
	g_movie.saved_restore = g_frontState.offscreenRestoreEnabled;
	g_movie.saved_lock = g_frontState.backBufferLocked;
	FrontendDisplay_UnlockBackBuffer();
	FrontendDisplay_DisableOffscreenRestore();
	FrontendDisplay_SetWndProcMode(2);
	FrontendText_ResetGlyphScratch();
	Keyboard_FlushCharBuffer();
	g_movieSkipRequested = 0;
	g_movie.synchronize =
		synchronize && g_frontendMissionSessionMode != FRONTEND_MISSION_SESSION_SINGLEPLAYER;
	g_moviePreviousWndProcMode = g_movie.saved_mode;
	if (g_movie.synchronize)
		XvtMovieSync_Begin();
	XvtPresentation_RequireClassic();
	g_movie.active = 1;
	g_movie.paused = 0;
	g_movie.result = 0;
	Aeron_LogInfo("xvt.movie", "Playing %s", g_movie.path);
	return XVT_MOVIE_PENDING;
}

static void XvtMovieTask_SubmitSubtitles(uint64_t frame, int bottom) {
	AeronPixelLayerDesc layer = { 0 };
	RECT clip;
	RECT rect;
	uint8_t* saved_pixels;
	int saved_pitch;
	int line;
	int waiting = g_movie.synchronize && g_moviePlaybackCompletionState;
	if ((!g_movieSubtitleFile && !g_movie.synchronize) || bottom <= 0)
		return;
	/* Each file record supplies the next boundary and the text for this interval. */
	while (!waiting && g_movieSubtitleFile && g_movie.next_cue != UINT16_MAX && g_movie.next_cue <= frame) {
		g_movie.next_cue = Movie_ReadSubtitleCue(g_movie.lines[0], g_movie.lines[1], g_movie.lines[2]);
		if (g_movie.next_cue == UINT16_MAX) {
			memset(g_movie.lines, 0, sizeof(g_movie.lines));
			break;
		}
	}
	memset(g_movie.overlay, 0, 640 * 480 * sizeof(uint16_t));
	saved_pixels = g_drawSurfacePtr;
	saved_pitch = g_frontState.drawSurfacePitch;
	FrontendDisplay_GetScreenClipRect(&clip);
	rect = (RECT) { 0, 0, 639, 479 };
	FrontendDisplay_SetScreenClipRect640x480(&rect);
	g_drawSurfacePtr = (uint8_t*)g_movie.overlay;
	g_frontState.drawSurfacePitch = 640 * sizeof(uint16_t);
	rect.top = 480 - bottom;
	for (line = 0; !waiting && line < 3; ++line) {
		rect.bottom = rect.top + bottom / 3;
		FrontendText_DrawCentered(12, g_movie.lines[line], &rect, 0xffff);
		rect.top = rect.bottom;
	}
	if (g_movie.synchronize)
		XvtMovieSync_Draw(bottom, bottom);
	g_drawSurfacePtr = saved_pixels;
	g_frontState.drawSurfacePitch = saved_pitch;
	FrontendDisplay_SetScreenClipRect640x480(&clip);
	layer.frame.pixels = g_movie.overlay;
	layer.frame.width = 640;
	layer.frame.height = 480;
	layer.frame.pitch = 1280;
	layer.frame.format = g_frontState.pixelFormat555 ? AERON_PIXEL_FORMAT_RGB555 : AERON_PIXEL_FORMAT_RGB565;
	layer.frame.color_space = AERON_COLOR_SPACE_SRGB;
	layer.frame.generation = ++g_movie.generation;
	layer.logical_rect = XvtPresentation_ClassicRect();
	layer.blend_mode = AERON_LAYER_BLEND_ALPHA;
	layer.color_key_enabled = 1;
	layer.color_key = 0;
	layer.preserve_encoded_values = 1;
	g_subtitleSubmission = Aeron_SubmitPixelLayer(&layer);
	if (!g_subtitleSubmission)
		Aeron_RequestFatalRendererError("movie subtitles");
}

static void XvtMovieTask_Submit(void) {
	AeronVideoPresentDesc desc = { 0 };
	AeronVideoInfo info;
	int width;
	int height;
	if (!Aeron_VideoGetInfo(g_movie.player, &info) || info.width <= 0 || info.height <= 0)
		return;
	width = info.width;
	/* Y-interlaced Smacker files decode at half height with a 1:2 sample aspect ratio. */
	height = info.display_width > 0 ? (int)((int64_t)info.width * info.display_height / info.display_width)
									: info.height;
	/* Original Smacker playback centers the native frame in the 640x480 display. */
	if (width > 640 || height > 480) {
		g_movie.result = 3;
		g_movie.active = 0;
		g_movie.complete = 1;
		return;
	}
	desc.bounds =
		XvtPresentation_FromClassic((AeronRectI) { (640 - width) / 2, (480 - height) / 2, width, height });
	desc.scale_mode = AERON_VIDEO_SCALE_CONTAIN;
	desc.blend_mode = AERON_LAYER_BLEND_OPAQUE;
	/* A skipped movie has no video frame, but its synchronization UI remains active. */
	if (Aeron_VideoSubmit(g_movie.player, &desc) || (g_movie.synchronize && g_moviePlaybackCompletionState)) {
		XvtRenderFrontend_Movie(1);
		XvtMovieTask_SubmitSubtitles(Aeron_VideoGetPresentedFrameIndex(g_movie.player), (480 - height) / 2);
		XvtRenderFrontend_Movie(0);
	}
}

void XvtMovieTask_Stop(void) {
	if (!g_movie.active)
		return;
	/* Skipping finishes local playback successfully; multiplayer still waits for peers. */
	Aeron_VideoStop(g_movie.player);
	if (g_movie.synchronize)
		XvtMovieSync_Wait();
}

void XvtMovieTask_Tick(void) {
	AeronVideoState state;
	int key;
	int synchronized = 0;
	if (!g_movie.active)
		return;
	if (g_movie.paused) {
		Aeron_VideoPlay(g_movie.player);
		g_movie.paused = 0;
	}
	Aeron_VideoUpdate(g_movie.player);
	key = (unsigned char)Keyboard_DequeueChar();
	if (g_movie.synchronize) {
		uint32_t playing = 1;
		if (key)
			Movie_MultiplayerFrameCallback(0, 0x102, key, 0, 0, &playing);
		if (g_frontState.mouseClickLatch || g_frontState.mouseRightClickLatch)
			Movie_MultiplayerFrameCallback(0, 0x202, 0, 0, 0, &playing);
		if (!playing)
			XvtMovieTask_Stop();
		synchronized = XvtMovieSync_Tick();
	} else if (key || g_frontState.mouseClickLatch || g_frontState.mouseRightClickLatch) {
		XvtMovieTask_Stop();
	}
	g_frontState.mouseClickLatch = g_frontState.mouseRightClickLatch = 0;
	state = Aeron_VideoGetState(g_movie.player);
	if (state == AERON_VIDEO_ERROR) {
		if (g_movie.result != 2)
			Aeron_LogError("xvt.movie", "%s: %s", g_movie.path, Aeron_VideoGetError(g_movie.player));
		g_movie.result = 2;
	} else {
		XvtMovieTask_Submit();
	}
	if (g_movie.synchronize && (state == AERON_VIDEO_ENDED || state == AERON_VIDEO_ERROR))
		XvtMovieSync_Wait();
	if ((!g_movie.synchronize && (state == AERON_VIDEO_ENDED || state == AERON_VIDEO_ERROR)) ||
		(g_movie.synchronize && (synchronized || g_movieSkipRequested == -1))) {
		if (g_movieSkipRequested == -1)
			g_movie.result = 5;
		g_movie.active = 0;
		g_movie.complete = 1;
		Aeron_LogInfo("xvt.movie", "Finished %s (result %d)", g_movie.name, g_movie.result);
	}
}

void XvtMovieTask_PausedFrame(void) {
	if (!g_movie.active)
		return;
	if (!g_movie.paused) {
		Aeron_VideoPause(g_movie.player);
		g_movie.paused = 1;
	}
	XvtMovieTask_Submit();
}

int XvtMovieTask_IsActive(void) { return g_movie.active; }

int XvtMovieTask_ContinuesWithoutFocus(void) { return g_movie.active && g_movie.synchronize; }

int XvtMovieTask_TakeResult(int* result) {
	if (!g_movie.complete || g_movie.player)
		return 0;
	*result = g_movie.result;
	g_movie.complete = 0;
	return 1;
}

uint64_t XvtMovieTask_NextWakeDelayUs(void) {
	uint64_t delay;
	return g_movie.active && Aeron_VideoGetNextWakeDelayUs(g_movie.player, &delay) ? delay : 10000;
}

void XvtMovieTask_Shutdown(void) {
	XvtMovieTask_Close();
	memset(&g_movie, 0, sizeof(g_movie));
}
