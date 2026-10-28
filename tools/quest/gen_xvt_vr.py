#!/usr/bin/env python3
"""Generate src/xvt_vr/xvt_vr.c from OpenXWA-VR's src/xwa_vr/xwa_vr.c.

The source is pinned: OpenXWA-VR commit XWA_BASE (v0.1.8-beta: radial menus,
no passthrough) for the host, vr_draw and the virtual keyboard, and OpenTIE-VR
commit TIE_BASE for vr_radial_menu.{c,h} (its entries are input actions, not
key chords: several XvT commands have no keyboard key). Both are read with
`git show`, so those checkouts may be on any commit. On top of OpenXWA's host
come the same controls as OpenTIE VR / OpenXW VR (XVT_PACKAGE_EDITS).

The OpenXR session, layers, laser pointer, virtual gamepad and virtual
keyboard are engine-level code shared with the X-Wing Alliance port (one APK
for Meta Quest and PICO); only the game hooks differ. This script renames the
API and swaps the XWA game queries for their OpenXvT equivalents. Re-run after
pulling OpenXWA-VR VR fixes; hand edits belong in the REPLACEMENTS table so
they survive regeneration. vr_keyboard.{c,h} and vr_keyboard_font.h are
copied with the same renames.
"""
import pathlib
import subprocess
import sys

XWA_REPO = pathlib.Path.home() / "Projekte/OpenXWA-VR"
XWA_BASE = "c69dedd"  # OpenXWA VR v0.1.8-beta
TIE_REPO = pathlib.Path.home() / "Projekte/OpenTIE-VR"
TIE_BASE = "09072f1"  # OpenTIE VR v0.1.8-beta
DST = pathlib.Path(__file__).resolve().parents[2] / "src/xvt_vr"
AUTOTEST = (pathlib.Path(__file__).resolve().parent / "xvt_vr_autotest.inc.c").read_text()

REPLACEMENTS = [
    ("/* Quest VR presentation for OpenXWA.",
     "/* Quest/PICO VR presentation for OpenXvT (shared with the X-Wing Alliance VR port)."),
    ('#include "xwa_vr/xwa_vr.h"', '#include "xvt_vr/xvt_vr.h"'),
    ('#include "xwa_remaster/vr_stereo.h"\n'
     '#include "xwa_vr/vr_radial_menu.h"\n'
     '#include "xwa_vr/vr_keyboard.h"\n'
     '#include "xwa/frontend/frontend_text.h"\n'
     '#include "xwa_runtime/runtime/flight_task.h"\n'
     '#include "xwa/flight/flight_display.h"\n'
     '#include "xwa/flight/hangar.h"\n'
     '#include "xwa/flight/player/player.h"\n',
     '#include "xvt_remaster/vr_stereo.h"\n'
     '#include "xvt_vr/vr_radial_menu.h"\n'
     '#include "xvt_vr/vr_keyboard.h"\n'
     '#include "xvt/frontend/frontend_text.h"\n'
     '#include "xvt_runtime/runtime/flight_task.h"\n'
     '#include "xvt_runtime/input/actions.h"\n'
     '#include "xvt_runtime/input/controller_mapping.h"\n'
     '#include "xvt_app/settings/settings.h"\n'),
    ('#define VR_LOG "xwa.vr"', '#define VR_LOG "xvt.vr"'),
    ("static int vr_in_flight(void) { return XwaFlightTask_IsActive(); }",
     "static int vr_in_flight(void) { return XvtFlightTask_IsActive(); }"),
    ("/* Screens inside a mission that only understand mouse and keyboard: the\n"
     " * hangar (Space launches, arrows/Enter drive the hangar menu) and the\n"
     " * Escape options screen. They get the menu controls, not the gamepad. */\n"
     "static int vr_flight_menu_active(void) {\n"
     "\treturn g_inHangarReady || FlightDisplay_IsFrontendModalActive();\n"
     "}",
     "/* Screens inside a mission that only understand mouse and keyboard: the\n"
     " * OpenXvT settings overlay. It gets the menu controls, not the gamepad. */\n"
     "static int vr_flight_menu_active(void) { return XvtSettingsMenu_Open(); }"),
    # XvT has no hangar launch: the left trigger types nothing in menus.
    ("\tvr_set_key(VR_KEY_LTRIGGER, SDL_SCANCODE_SPACE,\n"
     "\t\t\t   menu && g_inHangarReady && !FlightDisplay_IsFrontendModalActive() &&\n"
     "\t\t\t\t   vr_hysteresis(g_vr.trigger_down[VR_LEFT], g_vr.trigger[VR_LEFT]));\n",
     "\tvr_set_key(VR_KEY_LTRIGGER, SDL_SCANCODE_UNKNOWN, 0);\n"),
    ("type keys: A = Enter, B = Escape, Y = Space, left trigger = Space (hangar\n"
     " * only: launch), menu = Escape, left stick = arrows.",
     "type keys: A = Enter, B = Escape, Y = Space, menu = Escape,\n"
     " * left stick = arrows."),
    ("/* Menus (concourse, briefing, options, and in a mission the hangar and the\n"
     " * Escape options screen)",
     "/* Menus (concourse, briefing, options, and in a mission the settings\n"
     " * overlay)"),
    # Names
    ('"OpenXWA VR"', '"OpenXvT VR"'),
    ('SDL_strlcpy(set_info.actionSetName, "xwa"', 'SDL_strlcpy(set_info.actionSetName, "xvt"'),
    ('"X-Wing Alliance"', '"X-Wing vs. TIE Fighter"'),
    ('"OPENXWA_VR_DISABLE"', '"OPENXVT_VR_DISABLE"'),
    ("XwaVrStereoFrame", "XvtVrStereoFrame"),
    ("XwaVrStereoEye", "XvtVrStereoEye"),
    ("XwaRemasterVr_", "XvtRemasterVr_"),
    ("XwaVr_Install", "XvtVr_Install"),
    # OpenXvT may start SDL text input; on Quest that pops up the system
    # keyboard over the game (and Horizon refuses it in immersive mode).
    ("\tSDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, VR_PAD_ID_HINT);\n",
     "\tSDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, VR_PAD_ID_HINT);\n"
     "\tSDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, \"0\");\n"),
    # Log the first frame that reaches the virtual screen (bring-up aid).
    # Headless test script (tools/quest/xvt_vr_autotest.inc.c).
    ("static void vr_update_input(void) {\n", AUTOTEST + "static void vr_update_input(void) {\n"),
    ("\tg_vr.button_menu = vr_get_bool(g_vr.act_menu, XR_NULL_PATH);\n\n",
     "\tg_vr.button_menu = vr_get_bool(g_vr.act_menu, XR_NULL_PATH);\n\tvr_autotest_apply_inputs();\n\n"),
    ("\tvr_update_pointer();\n\tvr_update_mouse_buttons(flying);\n",
     "\tvr_update_pointer();\n\tvr_autotest_apply_pointer();\n\tvr_update_mouse_buttons(flying);\n"),
    ("\t\tg_vr.image_acquired      = 0;\n\t\tg_vr.image_ever_released = 1;\n",
     "\t\tg_vr.image_acquired = 0;\n"
     "\t\tif (!g_vr.image_ever_released) {\n"
     "\t\t\tAeron_LogInfo(VR_LOG, \"first frame presented to the virtual screen\");\n"
     "\t\t}\n"
     "\t\tg_vr.image_ever_released = 1;\n"),
    # A button still held when flight hands over to a menu (or back) only
    # counts again after release: otherwise the Menu press that opens the
    # in-flight settings overlay is read as Escape next frame and closes it,
    # and the Escape that closes it reopens it via the pad's Start.
    ("\tflying = in_flight && !vr_flight_menu_active();\n",
     "\tflying = in_flight && !vr_flight_menu_active();\n"
     "\t{\n"
     "\t\tstatic int prev_flying = -1, block_a, block_b, block_x, block_y, block_menu;\n"
     "\t\tif (prev_flying >= 0 && prev_flying != flying) {\n"
     "\t\t\tblock_a    = g_vr.button_a;\n"
     "\t\t\tblock_b    = g_vr.button_b;\n"
     "\t\t\tblock_x    = g_vr.button_x;\n"
     "\t\t\tblock_y    = g_vr.button_y;\n"
     "\t\t\tblock_menu = g_vr.button_menu;\n"
     "\t\t}\n"
     "\t\tprev_flying = flying;\n"
     "\t\tblock_a    = block_a && g_vr.button_a;\n"
     "\t\tblock_b    = block_b && g_vr.button_b;\n"
     "\t\tblock_x    = block_x && g_vr.button_x;\n"
     "\t\tblock_y    = block_y && g_vr.button_y;\n"
     "\t\tblock_menu = block_menu && g_vr.button_menu;\n"
     "\t\tg_vr.button_a    = g_vr.button_a && !block_a;\n"
     "\t\tg_vr.button_b    = g_vr.button_b && !block_b;\n"
     "\t\tg_vr.button_x    = g_vr.button_x && !block_x;\n"
     "\t\tg_vr.button_y    = g_vr.button_y && !block_y;\n"
     "\t\tg_vr.button_menu = g_vr.button_menu && !block_menu;\n"
     "\t}\n"),
]

# ---------------------------------------------------------------------------
# The OpenTIE VR / OpenXW VR control package on top of OpenXWA's host
# (applied after REPLACEMENTS).

# Code ranges dropped whole: [start, end) by unique markers. OpenXWA types its
# radial commands as key chords and has its own hyperspace logic (it reads XWA
# player state); XvT's radial entries are input actions (vr_run_command).
RANGE_REMOVALS = [
    ("/* Command menu shortcuts: how long Shift leads/trails and the key is held.\n",
     "/* Radial menus: a panel above the controller, facing the head. */\n"),
    ("/* Command menu shortcuts are typed one at a time: Shift down, key down,\n",
     "static void vr_release_all_keys(void) {\n"),
    ("\n\t/* Left stick click alone = hyperspace (prepare, then jump); with the\n",
     "}\n\n/* Uploads a CPU-drawn panel image into its swapchain. */\n"),
]

XVT_PACKAGE_EDITS = [
    # A new product id = a new pad model in config.yaml: OpenXvT stores the
    # gamepad bindings per pad when it first sees it, so 0.1.0 installs would
    # keep the old shift-layer buttons; the new id starts from the new
    # defaults (tools/quest/quest_config.py).
    ("#define VR_PAD_PRODUCT_ID 0x7857\n"
     "#define VR_PAD_ID_HINT    \"0x2833/0x7857\"\n",
     "#define VR_PAD_PRODUCT_ID 0x7858 /* 0.1.1: radial menu layout (0x7857 = 0.1.0) */\n"
     "#define VR_PAD_ID_HINT    \"0x2833/0x7858\"\n"),
    # --- leftovers of the chords, the right grip tap and the left click modes
    ("static const uint64_t VR_RGRIP_TAP_MS      = 250;   /* right grip: shorter = target in sight */\n"
     "static const uint64_t VR_RGRIP_PULSE_MS    = 100;\n", ""),
    ("/* The left stick click, decided when it goes down and kept until released. */\n"
     "enum { VR_LCLICK_NONE, VR_LCLICK_HYPERSPACE, VR_LCLICK_FIGHTER };\n\n", ""),
    ("\tint      suppressed;    /* this hold was used for hyperspace: choose nothing */\n",
     "\tint      suppressed;    /* this hold chooses nothing */\n"),
    ("\tint                   rgrip_held;      /* right grip down (flying) */\n"
     "\tuint64_t              rgrip_down_ms;   /* when it went down */\n"
     "\tuint64_t              rgrip_pulse_ms;  /* target-in-sight pulse lasts until */\n"
     "\tint                   lclick_mode;     /* what the held left stick click does (VR_LCLICK_*) */\n", ""),
    ("\t/* Queued keyboard shortcuts from the command menu */\n"
     "\tVrCommand             chord_queue[8];\n"
     "\tint                   chord_head, chord_count;\n"
     "\tVrCommand             chord_current;\n"
     "\tint                   chord_phase;\n"
     "\tuint64_t              chord_until_ms;\n", ""),
    ("\tvr_cancel_chords();\n", ""),
    ("\tvr_update_chords();\n", ""),
    # --- radial menus: entries are XvT input actions, dispatched like a pad
    # button bound to them (same queue and hold logic). No right grip tap:
    # holding either grip opens its star at once (target in sight is the left
    # trigger, as in the original games).
    ("\tvr_queue_chord(command);\n"
     "\tAeron_LogInfo(VR_LOG, \"radial menu: %s\", command->label);\n",
     "\tXvtControllerMapping_DispatchCommand((XvtInputAction)command->action, true);\n"
     "\tXvtControllerMapping_DispatchCommand((XvtInputAction)command->action, false);\n"
     "\tAeron_LogInfo(VR_LOG, \"radial menu: %s\", command->label);\n"),
    ("/* Radial menus in flight. Left grip held: energy/ship menu. Right grip: a\n"
     " * tap (< VR_RGRIP_TAP_MS, stick untouched) targets the craft in sight,\n"
     " * holding it opens the targeting/wingmen menu. The same hand's stick picks\n"
     " * an entry; releasing the grip runs it (stick centred = cancel). Left grip +\n"
     " * left stick click = hyperspace. */\n",
     "/* Radial menus in flight. Left grip held: weapons/power, right grip held:\n"
     " * targets/orders. The same hand's stick picks an entry; releasing the grip\n"
     " * runs it (stick centred = cancel). */\n"),
    ("\t\tif (hand == VR_RIGHT) {\n"
     "\t\t\tif (held && !g_vr.rgrip_held) {\n"
     "\t\t\t\tg_vr.rgrip_down_ms = now_ms;\n"
     "\t\t\t}\n"
     "\t\t\tif (!held && g_vr.rgrip_held && !radial->visible && flying) {\n"
     "\t\t\t\tg_vr.rgrip_pulse_ms = now_ms + VR_RGRIP_PULSE_MS; /* tap: target in sight */\n"
     "\t\t\t}\n"
     "\t\t\tg_vr.rgrip_held = held;\n"
     "\t\t\t/* Hold long enough, or start choosing, to open the menu. */\n"
     "\t\t\topen = held && (radial->visible || now_ms - g_vr.rgrip_down_ms >= VR_RGRIP_TAP_MS ||\n"
     "\t\t\t\t\t\t\tmag > VR_RADIAL_PICK);\n"
     "\t\t}\n", ""),
    # --- gamepad in flight: no grip buttons (the grips are the stars), stick
    # clicks plain (physical left = rightstick = forward view, physical right =
    # leftstick = hyperspace), pitch inverted like OpenTIE VR / OpenXW VR
    # (XvT flips gamepad pitch, so SDL down = stick up = nose down).
    (" *   left x (roll), triggers -> trigger axes, A/B/X/Y -> south/east/west/north,\n"
     " *   right grip -> right shoulder, physical left/right stick click -> right/left\n"
     " *   stick button, menu -> start.\n",
     " *   left x (roll), triggers -> trigger axes, A/B/X/Y -> south/east/west/north,\n"
     " *   physical left/right stick click -> right/left stick button, menu -> start.\n"),
    (" * half speed). The grips open the radial menus (vr_update_radials); a stick\n"
     " * choosing in its menu sends nothing to the pad, and a right grip tap pulses\n"
     " * the right shoulder (target in sight). */\n",
     " * half speed). The grips open the radial menus (vr_update_radials); a stick\n"
     " * choosing in its menu sends nothing to the pad. */\n"),
    ("\t\t\t\t\t\t\t   g_vr.radial[VR_LEFT].visible ? 0 : vr_axis(-right.y)); /* SDL: up is negative */\n",
     "\t\t\t\t\t\t\t   g_vr.radial[VR_LEFT].visible ? 0 : vr_axis(right.y)); /* inverted pitch: stick up = SDL down = nose down */\n"),
    ("\t/* Right grip tap = target in sight (a short pulse on release); holding\n"
     "\t * it opens the radial menu instead. */\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GetTicks() < g_vr.rgrip_pulse_ms);\n",
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0);\n"),
    ("\t/* Left grip + left stick click = right stick button (nearest fighter by\n"
     "\t * default); the click alone is hyperspace (vr_update_radials). */\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK,\n"
     "\t\t\t\t\t\t\t\t g_vr.lclick_mode == VR_LCLICK_FIGHTER && g_vr.stick_click[VR_LEFT]);\n",
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK, g_vr.stick_click[VR_LEFT]);\n"),
    ("\t/* Unused since the command menu replaced the left-grip shift layer. */\n",
     "\t/* Unused since the radial menus replaced the left-grip shift layer. */\n"),
    # --- menus, left controller only in a mission (the right one is
    # distracting there); in the main menus either controller is the mouse.
    # Game menus: laser + trigger clicks, the laser hand's grip right-clicks,
    # X = Enter, Y = Space, left stick = arrow keys, Menu = Escape. The OpenXvT
    # settings overlay: gamepad only (no laser), left controller: stick =
    # focus, X = select, Y = back, left trigger / grip = previous / next page.
    ("\tg_vr.pointer_hand   = VR_RIGHT;\n", "\tg_vr.pointer_hand   = VR_LEFT;\n"),
    ("\t/* The hand that last pulled its trigger owns the pointer. */\n"
     "\tfor (int hand = 0; hand < VR_HAND_COUNT; ++hand) {\n"
     "\t\tif (g_vr.trigger[hand] > 0.65f && !g_vr.trigger_down[hand]) {\n"
     "\t\t\tg_vr.pointer_hand = (VrHand)hand;\n"
     "\t\t}\n"
     "\t}\n"
     "\t/* A ray on the virtual keyboard doesn't also point at the screen. */\n"
     "\tg_vr.pointer_valid = !g_vr.kb_hit[g_vr.pointer_hand] && vr_pointer_hit(g_vr.pointer_hand, &x, &y);\n",
     "\t/* No laser in the OpenXvT settings overlay (gamepad only there). In the\n"
     "\t * main menus either controller is the mouse (the hand that last pulled\n"
     "\t * its trigger); in a mission only the left one. */\n"
     "\tif (vr_in_flight()) {\n"
     "\t\tg_vr.pointer_hand = VR_LEFT;\n"
     "\t} else {\n"
     "\t\tfor (int hand = 0; hand < VR_HAND_COUNT; ++hand) {\n"
     "\t\t\tif (g_vr.trigger[hand] > 0.65f && !g_vr.trigger_down[hand]) {\n"
     "\t\t\t\tg_vr.pointer_hand = (VrHand)hand;\n"
     "\t\t\t}\n"
     "\t\t}\n"
     "\t}\n"
     "\t/* A ray on the virtual keyboard doesn't also point at the screen. */\n"
     "\tg_vr.pointer_valid = !XvtSettingsMenu_Open() && !g_vr.kb_hit[g_vr.pointer_hand] &&\n"
     "\t\t\t\t\t\t vr_pointer_hit(g_vr.pointer_hand, &x, &y);\n"),
    ("\tconst int    left_down  = !in_flight && vr_hysteresis(g_vr.mouse_left_down, g_vr.trigger[hand]);\n"
     "\t/* In flight the right grip targets; in menus it right-clicks. */\n"
     "\tconst int    right_down = !in_flight && vr_hysteresis(g_vr.mouse_right_down, g_vr.grip[VR_RIGHT]);\n",
     "\tconst int    laser      = !in_flight && !XvtSettingsMenu_Open();\n"
     "\tconst int    left_down  = laser && vr_hysteresis(g_vr.mouse_left_down, g_vr.trigger[hand]);\n"
     "\t/* In game menus the laser hand's grip right-clicks (in flight the grips\n"
     "\t * open the radial menus). */\n"
     "\tconst int    right_down = laser && vr_hysteresis(g_vr.mouse_right_down, g_vr.grip[hand]);\n"),
    ("\tif (!in_flight) {\n"
     "\t\t/* Menus own the controllers: report a fully neutral pad. The lever\n"
     "\t\t * position is kept in g_vr.throttle and reported again (as a change)\n"
     "\t\t * when flight resumes. Reporting it here made the in-flight options\n"
     "\t\t * menu, which navigates with the left stick axes, scroll forever. */\n"
     "\t\tfor (int b = 0; b <= SDL_GAMEPAD_BUTTON_MISC1; ++b) {\n"
     "\t\t\tSDL_SetJoystickVirtualButton(g_vr.pad, b, 0);\n"
     "\t\t}\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTX, 0);\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTY, 0);\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTX, 0);\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTY, 0);\n",
     "\tif (!in_flight) {\n"
     "\t\t/* Menus. The throttle lever is never reported here: it stays in\n"
     "\t\t * g_vr.throttle and comes back (as a change) when flight resumes\n"
     "\t\t * (reporting it made the settings list scroll forever). Game menus get\n"
     "\t\t * a neutral pad (the laser is the pointer). The OpenXvT settings\n"
     "\t\t * overlay is gamepad only, left controller: stick = focus, X = select\n"
     "\t\t * (south), Y = back (east), left trigger / grip = previous / next page\n"
     "\t\t * (shoulders). */\n"
     "\t\tconst int        settings   = XvtSettingsMenu_Open();\n"
     "\t\tconst XrVector2f menu_stick = g_vr.stick[VR_LEFT];\n"
     "\t\tfor (int b = 0; b <= SDL_GAMEPAD_BUTTON_MISC1; ++b) {\n"
     "\t\t\tSDL_SetJoystickVirtualButton(g_vr.pad, b, 0);\n"
     "\t\t}\n"
     "\t\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_SOUTH, settings && g_vr.button_x);\n"
     "\t\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_EAST, settings && g_vr.button_y);\n"
     "\t\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,\n"
     "\t\t\t\t\t\t\t\t\t settings && vr_hysteresis(g_vr.trigger_down[VR_LEFT], g_vr.trigger[VR_LEFT]));\n"
     "\t\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,\n"
     "\t\t\t\t\t\t\t\t\t settings && vr_hysteresis(g_vr.grip_down[VR_LEFT], g_vr.grip[VR_LEFT]));\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTX, 0);\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTY, 0);\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTX, settings ? vr_axis(menu_stick.x) : 0);\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTY, settings ? vr_axis(-menu_stick.y) : 0);\n"),
    (" * type keys: A = Enter, B = Escape, Y = Space, menu = Escape,\n"
     " * left stick = arrows.",
     " * type keys (left controller): X = Enter, Y = Space, menu = Escape,\n"
     " * left stick = arrows; the OpenXvT settings overlay gets the gamepad."),
    ("\tconst int   menu = !in_flight;\n",
     "\tconst int   menu = !in_flight && !XvtSettingsMenu_Open(); /* settings overlay: pad only */\n"),
    ("\tvr_set_key(VR_KEY_A, SDL_SCANCODE_RETURN, menu && g_vr.button_a);\n"
     "\tvr_set_key(VR_KEY_B, SDL_SCANCODE_ESCAPE, menu && g_vr.button_b);\n"
     "\tvr_set_key(VR_KEY_X, SDL_SCANCODE_UNKNOWN, 0);\n",
     "\tvr_set_key(VR_KEY_A, SDL_SCANCODE_UNKNOWN, 0);\n"
     "\tvr_set_key(VR_KEY_B, SDL_SCANCODE_UNKNOWN, 0);\n"
     "\tvr_set_key(VR_KEY_X, SDL_SCANCODE_RETURN, menu && g_vr.button_x);\n"),
    # Menu still types Escape in the settings overlay: that closes it.
    ("\tvr_set_key(VR_KEY_MENU, SDL_SCANCODE_ESCAPE, menu && g_vr.button_menu);\n",
     "\tvr_set_key(VR_KEY_MENU, SDL_SCANCODE_ESCAPE, !in_flight && g_vr.button_menu);\n"),
]

# vr_radial_menu.{c,h} from OpenTIE VR, XvT's commands at OpenTIE's
# positions (all of them exist in XvT under the same names).
RADIAL_RENAMES = [
    ("tie_vr/", "xvt_vr/"),
    ("TIE_VR_RADIAL_MENU_H", "XVT_VR_RADIAL_MENU_H"),
    ("tie_runtime/input/actions.h", "xvt_runtime/input/actions.h"),
    ("TieInputAction", "XvtInputAction"),
    ("TIE_INPUT_ACTION_", "XVT_INPUT_ACTION_"),
    ("tie_vr.c", "xvt_vr.c"),
    ("Same layout logic as OpenXWA-VR's vr_radial_menu.c.", "Shared with OpenTIE-VR (tools/quest/gen_xvt_vr.py)."),
]

SIMPLE = [
    ("xwa_vr/", "xvt_vr/"),
    ("XWA_VR_", "XVT_VR_"),
    ("xwa_vr.c", "xvt_vr.c"),
    ("XwaVr_", "XvtVr_"),
    ("OpenXWA", "OpenXvT"),
]


def xwa(name: str) -> str:
    return subprocess.run(["git", "-C", str(XWA_REPO), "show", f"{XWA_BASE}:src/xwa_vr/{name}"],
                          check=True, capture_output=True, text=True).stdout


def tie(name: str) -> str:
    return subprocess.run(["git", "-C", str(TIE_REPO), "show", f"{TIE_BASE}:src/tie_vr/{name}"],
                          check=True, capture_output=True, text=True).stdout


def convert(text: str, table) -> str:
    for old, new in table:
        if old not in text:
            raise SystemExit(f"pattern not found: {old[:70]!r}")
        text = text.replace(old, new)
    return text


def remove_ranges(text: str) -> str:
    for start, end in RANGE_REMOVALS:
        if text.count(start) != 1:
            raise SystemExit(f"range start not unique: {start[:70]!r}")
        a = text.index(start)
        b = text.find(end, a)
        if b < 0:
            raise SystemExit(f"range end not found: {end[:70]!r}")
        text = text[:a] + text[b:]
    return text


def main() -> int:
    DST.mkdir(parents=True, exist_ok=True)
    text = convert(xwa("xwa_vr.c"), REPLACEMENTS)
    text = convert(remove_ranges(text), XVT_PACKAGE_EDITS)
    leftovers = [w for w in ("Xwa", "xwa", "XWA", "g_inHangar", "chord", "g_vr.rgrip", "lclick_mode", "VR_LCLICK") if w in text]
    if leftovers:
        print(f"unconverted identifiers remain in xvt_vr.c: {leftovers}", file=sys.stderr)
        return 1
    (DST / "xvt_vr.c").write_text(text)

    header = xwa("xwa_vr.h")
    for old, new in SIMPLE:
        header = header.replace(old, new)
    (DST / "xvt_vr.h").write_text(header)
    for name in ("vr_keyboard.c", "vr_keyboard.h", "vr_keyboard_font.h", "vr_draw.c", "vr_draw.h"):
        body = xwa(name)
        for old, new in SIMPLE:
            body = body.replace(old, new)
        if any(w in body for w in ("Xwa", "xwa_", "XWA_")):
            print(f"unconverted identifiers remain in {name}", file=sys.stderr)
            return 1
        (DST / name).write_text(body)
    for name in ("vr_radial_menu.c", "vr_radial_menu.h"):
        body = tie(name)
        for old, new in RADIAL_RENAMES:
            body = body.replace(old, new)
        if any(w in body for w in ("Tie", "tie_", "TIE_")):
            print(f"unconverted identifiers remain in {name}", file=sys.stderr)
            return 1
        (DST / name).write_text(body)
    print(f"wrote {DST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
