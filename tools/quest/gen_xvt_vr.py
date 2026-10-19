#!/usr/bin/env python3
"""Generate src/xvt_vr/xvt_vr.c from OpenXWA-VR's src/xwa_vr/xwa_vr.c.

The OpenXR session, layers, laser pointer, virtual gamepad and virtual
keyboard are engine-level code shared with the X-Wing Alliance port (one APK
for Meta Quest and PICO); only the game hooks differ. This script renames the
API and swaps the XWA game queries for their OpenXvT equivalents. Re-run after
pulling OpenXWA-VR VR fixes; hand edits belong in the REPLACEMENTS table so
they survive regeneration. vr_keyboard.{c,h} and vr_keyboard_font.h are
copied with the same renames.
"""
import pathlib
import sys

XWA = pathlib.Path.home() / "Projekte/OpenXWA-VR/src/xwa_vr"
DST = pathlib.Path(__file__).resolve().parents[2] / "src/xvt_vr"
AUTOTEST = (pathlib.Path(__file__).resolve().parent / "xvt_vr_autotest.inc.c").read_text()

REPLACEMENTS = [
    ("/* Quest VR presentation for OpenXWA.",
     "/* Quest/PICO VR presentation for OpenXvT (shared with the X-Wing Alliance VR port)."),
    ('#include "xwa_vr/xwa_vr.h"', '#include "xvt_vr/xvt_vr.h"'),
    ('#include "xwa_remaster/vr_stereo.h"\n'
     '#include "xwa_vr/vr_keyboard.h"\n'
     '#include "xwa/frontend/frontend_text.h"\n'
     '#include "xwa_runtime/runtime/flight_task.h"\n'
     '#include "xwa/flight/flight_display.h"\n'
     '#include "xwa/flight/hangar.h"\n',
     '#include "xvt_remaster/vr_stereo.h"\n'
     '#include "xvt_vr/vr_keyboard.h"\n'
     '#include "xvt/frontend/frontend_text.h"\n'
     '#include "xvt_runtime/runtime/flight_task.h"\n'
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
    # The settings overlay (Aeron UI) navigates with the pad's left stick, so a
    # throttle lever parked on LEFTY scrolled its focus endlessly: menus get a
    # fully neutral pad. The lever comes back with the first flying frame.
    ("\t\t/* Menus own the controllers: report a neutral pad (lever kept). */\n",
     "\t\t/* Menus own the controllers: report a neutral pad (the lever too; the\n"
     "\t\t * settings overlay navigates with the left stick). */\n"),
    ("\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTY, vr_axis(g_vr.throttle * 2.0f - 1.0f));\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER",
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTY, 0);\n"
     "\t\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER"),
]

SIMPLE = [
    ("xwa_vr/", "xvt_vr/"),
    ("XWA_VR_", "XVT_VR_"),
    ("xwa_vr.c", "xvt_vr.c"),
    ("XwaVr_", "XvtVr_"),
    ("OpenXWA", "OpenXvT"),
]


def convert(text: str, table) -> str:
    for old, new in table:
        if old not in text:
            raise SystemExit(f"pattern not found: {old[:70]!r}")
        text = text.replace(old, new)
    return text


def main() -> int:
    DST.mkdir(parents=True, exist_ok=True)
    text = convert((XWA / "xwa_vr.c").read_text(), REPLACEMENTS)
    leftovers = [w for w in ("Xwa", "xwa", "XWA", "g_inHangar") if w in text]
    if leftovers:
        print(f"unconverted identifiers remain in xvt_vr.c: {leftovers}", file=sys.stderr)
        return 1
    (DST / "xvt_vr.c").write_text(text)

    header = (XWA / "xwa_vr.h").read_text()
    for old, new in SIMPLE:
        header = header.replace(old, new)
    (DST / "xvt_vr.h").write_text(header)
    for name in ("vr_keyboard.c", "vr_keyboard.h", "vr_keyboard_font.h"):
        body = (XWA / name).read_text()
        for old, new in SIMPLE:
            body = body.replace(old, new)
        if any(w in body for w in ("Xwa", "xwa_", "XWA_")):
            print(f"unconverted identifiers remain in {name}", file=sys.stderr)
            return 1
        (DST / name).write_text(body)
    print(f"wrote {DST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
