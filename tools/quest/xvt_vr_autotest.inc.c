/* ------------------------------------------------------------------------ */
/* Headless test script (bring-up aid; tools/quest/xvt_vr_autotest.inc.c)   */
/*
 * When /sdcard/OpenXvT/vr_autotest.txt exists, its commands drive the Touch
 * inputs once after start-up, so menus and flight can be tested without
 * someone wearing the headset. One command per line:
 *   wait <seconds>
 *   point <x> <y>        laser on the virtual screen, 1920x1080 pixels
 *                        (the 640x480 frontend spans x 240..1680)
 *   point off            give the laser back to the controllers
 *   press <input> <s>    hold a | b | x | y | menu | rtrigger | ltrigger |
 *                        rgrip | lgrip | lclick | rclick for <s> seconds
 *   stick <l|r> <x> <y> <s>  hold a stick deflection
 *   log <text>           mark a step in logcat (tag xvt.vr)
 * Without the file nothing changes. */

#define VR_AUTOTEST_PATH      "/sdcard/OpenXvT/vr_autotest.txt"
#define VR_AUTOTEST_MAX_LINES 256

static struct {
	int      loaded, active, line, line_count;
	char*    text;
	char*    lines[VR_AUTOTEST_MAX_LINES];
	uint64_t step_end_ns;
	int      point_on;
	float    point_x, point_y;
	char     press[16];
	int      stick_hand;
	float    stick_x, stick_y;
	int      holding;
} g_vrt;

static void vr_autotest_load(void) {
	size_t size = 0;
	g_vrt.loaded = 1;
	g_vrt.text   = (char*)SDL_LoadFile(VR_AUTOTEST_PATH, &size);
	if (!g_vrt.text) {
		return;
	}
	for (char* p = g_vrt.text; *p && g_vrt.line_count < VR_AUTOTEST_MAX_LINES;) {
		char* end = SDL_strchr(p, '\n');
		if (end) {
			*end = '\0';
		}
		g_vrt.lines[g_vrt.line_count++] = p;
		if (!end) {
			break;
		}
		p = end + 1;
	}
	g_vrt.active = 1;
	Aeron_LogInfo(VR_LOG, "autotest: %d lines from %s", g_vrt.line_count, VR_AUTOTEST_PATH);
}

/* Runs the script; returns while a timed step (wait/press/stick) is pending. */
static void vr_autotest_step(void) {
	const uint64_t now = SDL_GetTicksNS();
	if (!g_vrt.loaded) {
		vr_autotest_load();
	}
	if (!g_vrt.active || now < g_vrt.step_end_ns) {
		return;
	}
	g_vrt.holding  = 0;
	g_vrt.press[0] = '\0';
	while (g_vrt.line < g_vrt.line_count) {
		char  cmd[16] = { 0 }, arg[64] = { 0 };
		float a = 0.0f, b = 0.0f, c = 0.0f;
		const char* line = g_vrt.lines[g_vrt.line++];
		if (SDL_sscanf(line, "%15s", cmd) != 1 || cmd[0] == '#') {
			continue;
		}
		if (!SDL_strcmp(cmd, "log")) {
			Aeron_LogInfo(VR_LOG, "autotest: %s", line + 3);
		} else if (!SDL_strcmp(cmd, "wait") && SDL_sscanf(line, "%*s %f", &a) == 1) {
			g_vrt.step_end_ns = now + (uint64_t)(a * 1e9f);
			return;
		} else if (!SDL_strcmp(cmd, "point")) {
			if (SDL_sscanf(line, "%*s %f %f", &a, &b) == 2) {
				g_vrt.point_on = 1;
				g_vrt.point_x  = a;
				g_vrt.point_y  = b;
			} else {
				g_vrt.point_on = 0;
			}
		} else if (!SDL_strcmp(cmd, "press") && SDL_sscanf(line, "%*s %15s %f", arg, &a) == 2) {
			SDL_strlcpy(g_vrt.press, arg, sizeof g_vrt.press);
			g_vrt.holding     = 1;
			g_vrt.step_end_ns = now + (uint64_t)(a * 1e9f);
			return;
		} else if (!SDL_strcmp(cmd, "stick") && SDL_sscanf(line, "%*s %63s %f %f %f", arg, &a, &b, &c) == 4) {
			g_vrt.stick_hand  = arg[0] == 'l' ? VR_LEFT : VR_RIGHT;
			g_vrt.stick_x     = a;
			g_vrt.stick_y     = b;
			g_vrt.holding     = 2;
			g_vrt.step_end_ns = now + (uint64_t)(c * 1e9f);
			return;
		} else {
			Aeron_LogWarn(VR_LOG, "autotest: bad line '%s'", line);
		}
	}
	Aeron_LogInfo(VR_LOG, "autotest: done");
	g_vrt.active   = 0;
	g_vrt.point_on = 0;
}

/* Overrides the controller state read this frame. */
static void vr_autotest_apply_inputs(void) {
	vr_autotest_step();
	if (g_vrt.holding == 2) {
		g_vr.stick[g_vrt.stick_hand].x = g_vrt.stick_x;
		g_vr.stick[g_vrt.stick_hand].y = g_vrt.stick_y;
	} else if (g_vrt.holding == 1) {
		const char* p = g_vrt.press;
		if (!SDL_strcmp(p, "a")) g_vr.button_a = 1;
		else if (!SDL_strcmp(p, "b")) g_vr.button_b = 1;
		else if (!SDL_strcmp(p, "x")) g_vr.button_x = 1;
		else if (!SDL_strcmp(p, "y")) g_vr.button_y = 1;
		else if (!SDL_strcmp(p, "menu")) g_vr.button_menu = 1;
		else if (!SDL_strcmp(p, "rtrigger")) g_vr.trigger[VR_RIGHT] = 1.0f;
		else if (!SDL_strcmp(p, "ltrigger")) g_vr.trigger[VR_LEFT] = 1.0f;
		else if (!SDL_strcmp(p, "rgrip")) g_vr.grip[VR_RIGHT] = 1.0f;
		else if (!SDL_strcmp(p, "lgrip")) g_vr.grip[VR_LEFT] = 1.0f;
		else if (!SDL_strcmp(p, "lclick")) g_vr.stick_click[VR_LEFT] = 1;
		else if (!SDL_strcmp(p, "rclick")) g_vr.stick_click[VR_RIGHT] = 1;
	}
}

/* Overrides the laser hit while a "point" is active. */
static void vr_autotest_apply_pointer(void) {
	if (!g_vrt.point_on) {
		return;
	}
	g_vr.pointer_hand  = VR_RIGHT;
	g_vr.pointer_valid = 1;
	g_vr.pointer_x     = g_vrt.point_x;
	g_vr.pointer_y     = g_vrt.point_y;
	Aeron_SetExternalPointer(1, g_vr.pointer_x, g_vr.pointer_y);
}

