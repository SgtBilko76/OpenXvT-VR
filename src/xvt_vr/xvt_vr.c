/* Quest/PICO VR presentation for OpenXvT (shared with the X-Wing Alliance VR port).
 *
 * The game keeps rendering a normal flat frame; Aeron composes that frame into
 * an OpenXR swapchain (external presentation hooks) which is shown as a large
 * virtual screen in front of the player, over black in the menus and over
 * space in flight.
 *
 * Touch controllers:
 *  - a virtual SDL gamepad (sticks, throttle, fire, targeting) that the
 *    game's existing controller support picks up,
 *  - in-flight radial menus (vr_radial_menu.c) on both grips that type the
 *    game's keyboard shortcuts for everything else,
 *  - a laser pointer + clicks for the mouse-driven menus,
 *  - a virtual keyboard (vr_keyboard.c) while a text field takes input.
 */
#include "xvt_vr/xvt_vr.h"

#if defined(__ANDROID__)

#define XR_NO_PROTOTYPES 1
#include <openxr/openxr.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_openxr.h>

#include "aeron/aeron.h"
#include "aeron/external_present.h"
#include "xvt_remaster/vr_stereo.h"
#include "xvt_vr/vr_radial_menu.h"
#include "xvt_vr/vr_keyboard.h"
#include "xvt/frontend/frontend_text.h"
#include "xvt_runtime/runtime/flight_task.h"
#include "xvt_runtime/input/actions.h"
#include "xvt_runtime/input/controller_mapping.h"
#include "xvt_app/settings/settings.h"

#include <math.h>
#include <string.h>

#define VR_LOG "xvt.vr"

enum {
	VR_SCREEN_WIDTH_PX  = 1920,
	VR_SCREEN_HEIGHT_PX = 1080,
};

/* USB IDs of the virtual Touch gamepad. Android also exposes the Touch
 * controllers as plain gamepads that never report input inside an immersive
 * OpenXR session; SDL is told to ignore every gamepad but this one. */
#define VR_REFRESH_HZ 72.0f

#define VR_PAD_VENDOR_ID  0x2833 /* Oculus VR */
#define VR_PAD_PRODUCT_ID 0x7858 /* 0.1.1: radial menu layout (0x7857 = 0.1.0) */
#define VR_PAD_ID_HINT    "0x2833/0x7858"

/* Virtual screen: metres in LOCAL space (origin = head at recenter). */
static const float VR_SCREEN_DISTANCE     = 2.5f;
static const float VR_SCREEN_WIDTH_MENU   = 3.2f;
static const float VR_SCREEN_WIDTH_FLIGHT = 4.0f;
/* Stereo flight: eye resolution as a fraction of the runtime's recommendation,
 * and the distance of the HUD overlay (its angular size matches the game
 * camera, so the distance only sets its stereo depth). */
static const float VR_EYE_SCALE    = 0.80f;
static const float VR_HUD_DISTANCE = 3.0f;
/* Virtual keyboard: a desk-like panel below the screen, tilted towards the
 * player, shown in menus while a text field (pilot name...) takes input. */
static const float    VR_KB_DISTANCE   = 1.4f;
static const float    VR_KB_HEIGHT     = -0.55f;
static const float    VR_KB_TILT_DEG   = -35.0f;
static const float    VR_KB_WIDTH_M    = 1.1f;
static const uint64_t VR_KB_LINGER_MS  = 300; /* hide after the field stops asking for text */
/* Radial menus: a panel above the controller, facing the head. */
static const float    VR_RADIAL_SIZE_M     = 0.30f;
static const float    VR_RADIAL_LIFT_M     = 0.17f; /* panel centre above the controller */
static const float    VR_RADIAL_PICK       = 0.55f; /* stick deflection that selects */
static const float    VR_RADIAL_KEEP       = 0.30f; /* ...and keeps the selection */
static const uint64_t VR_RADIAL_LINGER_MS  = 250;   /* selection survives the stick springing back */

/* A quad layer with its own swapchain, drawn on the CPU (radial menus). */
typedef struct VrPanel {
	XrSwapchain            swapchain;
	SDL_GPUTexture**       images;
	SDL_GPUTransferBuffer* transfer;
	uint8_t*               pixels;
	int                    width, height;
	int                    ever_released;
	int                    dirty;
} VrPanel;

typedef struct VrRadialState {
	VrPanel  panel;
	int      visible;
	int      selected;      /* VR_RADIAL_NONE or item */
	int      drawn_selected;
	int      suppressed;    /* this hold chooses nothing */
	uint64_t selected_ms;   /* last time the stick pointed at `selected` */
	XrPosef  pose;          /* panel pose in LOCAL space */
	int      pose_valid;
} VrRadialState;

typedef enum VrHand { VR_LEFT = 0, VR_RIGHT = 1, VR_HAND_COUNT = 2 } VrHand;

/* Keyboard shortcuts injected as SDL key events, one slot per Touch input. */
typedef enum VrKeySlot {
	VR_KEY_A,
	VR_KEY_B,
	VR_KEY_X,
	VR_KEY_Y,
	VR_KEY_MENU,
	VR_KEY_LSTICK_CLICK,
	VR_KEY_RSTICK_CLICK,
	VR_KEY_LTRIGGER,
	VR_KEY_RGRIP_MENU,
	VR_KEY_UP,
	VR_KEY_DOWN,
	VR_KEY_LEFT,
	VR_KEY_RIGHT,
	VR_KEY_SLOT_COUNT
} VrKeySlot;

#define XR_FN(name) static PFN_##name p_##name;
#define XR_FUNCTIONS(X)                    \
	X(xrPollEvent)                         \
	X(xrBeginSession)                      \
	X(xrEndSession)                        \
	X(xrRequestExitSession)                \
	X(xrDestroySession)                    \
	X(xrWaitFrame)                         \
	X(xrBeginFrame)                        \
	X(xrEndFrame)                          \
	X(xrCreateReferenceSpace)              \
	X(xrDestroySpace)                      \
	X(xrLocateSpace)                       \
	X(xrAcquireSwapchainImage)             \
	X(xrWaitSwapchainImage)                \
	X(xrReleaseSwapchainImage)             \
	X(xrStringToPath)                      \
	X(xrCreateActionSet)                   \
	X(xrCreateAction)                      \
	X(xrSuggestInteractionProfileBindings) \
	X(xrAttachSessionActionSets)           \
	X(xrSyncActions)                       \
	X(xrGetActionStateBoolean)             \
	X(xrGetActionStateFloat)               \
	X(xrGetActionStateVector2f)            \
	X(xrGetActionStatePose)                \
	X(xrCreateActionSpace)                 \
	X(xrApplyHapticFeedback)               \
	X(xrStopHapticFeedback)                \
	X(xrEnumerateInstanceExtensionProperties) \
	X(xrEnumerateViewConfigurationViews)       \
	X(xrLocateViews)
XR_FUNCTIONS(XR_FN)
static PFN_xrGetInstanceProcAddr       p_xrGetInstanceProcAddr;
static PFN_xrGetDisplayRefreshRateFB   p_xrGetDisplayRefreshRateFB;
static PFN_xrRequestDisplayRefreshRateFB p_xrRequestDisplayRefreshRateFB;

static struct {
	/* OpenXR */
	XrInstance            instance;
	XrSystemId            system;
	XrSession             session;
	XrSpace               local_space;
	XrSwapchain           swapchain;
	SDL_GPUTexture**      images;
	SDL_GPUDevice*        device;
	XrSessionState        state;
	int                   running;
	int                   focused;
	int                   quit_pushed;
	XrFrameState          frame_state;
	int                   frame_begun;
	int                   image_acquired;
	int                   image_ever_released;
	float                 refresh_hz;
	/* Stereo eyes */
	XrSwapchain           eye_swapchain[2];
	SDL_GPUTexture**      eye_images[2];
	int                   eye_width, eye_height;
	int                   eye_acquired[2];
	XrView                views[2];
	int                   views_valid;
	int                   stereo_published;
	float                 screen_width_m;
	/* Input */
	XrActionSet           action_set;
	XrAction              act_aim;
	XrAction              act_trigger;
	XrAction              act_grip;
	XrAction              act_stick;
	XrAction              act_stick_click;
	XrAction              act_haptic;
	XrAction              act_a, act_b, act_x, act_y, act_menu;
	XrPath                hand_path[VR_HAND_COUNT];
	XrSpace               aim_space[VR_HAND_COUNT];
	float                 trigger[VR_HAND_COUNT];
	float                 grip[VR_HAND_COUNT];
	XrVector2f            stick[VR_HAND_COUNT];
	int                   stick_click[VR_HAND_COUNT];
	int                   has_bd_controllers;
	int                   button_a, button_b, button_x, button_y, button_menu;
	/* Pointer */
	VrHand                pointer_hand;
	int                   pointer_valid;
	float                 pointer_x, pointer_y;
	int                   mouse_left_down, mouse_right_down;
	int                   trigger_down[VR_HAND_COUNT];
	int                   grip_down[VR_HAND_COUNT];
	uint64_t              last_click_ms;
	float                 last_click_x, last_click_y;
	/* Virtual gamepad */
	SDL_JoystickID        pad_id;
	SDL_Joystick*         pad;
	float                 throttle;
	uint64_t              last_input_ns;
	uint16_t              rumble_low, rumble_high;
	/* Injected keys currently held, per slot */
	SDL_Scancode          held_key[VR_KEY_SLOT_COUNT];
	/* Virtual keyboard */
	XrSwapchain           kb_swapchain;
	SDL_GPUTexture**      kb_images;
	SDL_GPUTransferBuffer* kb_transfer;
	uint8_t*              kb_pixels;
	int                   kb_bgra;
	int                   kb_ever_released;
	int                   kb_visible;
	int                   kb_dirty;
	int                   kb_drawn_flash;
	unsigned int          kb_serial;
	uint64_t              kb_active_ms;
	VrKeyboard            kb;
	int                   kb_hit[VR_HAND_COUNT]; /* ray on the panel (keyboard or command menu) */
	int                   kb_held_key[VR_HAND_COUNT];
	uint64_t              kb_repeat_ms[VR_HAND_COUNT];
	/* In-flight radial menus, one per hand */
	VrRadialState         radial[VR_HAND_COUNT];
	int                   panel_bgra;
	XrVector3f            head_pos;        /* last known head position (LOCAL) */
} g_vr;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */

static int vr_ok(XrResult result, const char* what) {
	if (XR_FAILED(result)) {
		Aeron_LogError(VR_LOG, "%s failed: %d", what, (int)result);
		return 0;
	}
	return 1;
}

static XrPath vr_path(const char* text) {
	XrPath path = XR_NULL_PATH;
	p_xrStringToPath(g_vr.instance, text, &path);
	return path;
}

static XrVector3f vr_rotate(XrQuaternionf q, XrVector3f v) {
	/* v' = v + 2w(u x v) + 2(u x (u x v)), u = q.xyz */
	const XrVector3f u  = { q.x, q.y, q.z };
	XrVector3f       t  = { 2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z),
							2.0f * (u.x * v.y - u.y * v.x) };
	XrVector3f       r;
	r.x = v.x + q.w * t.x + (u.y * t.z - u.z * t.y);
	r.y = v.y + q.w * t.y + (u.z * t.x - u.x * t.z);
	r.z = v.z + q.w * t.z + (u.x * t.y - u.y * t.x);
	return r;
}

static int vr_in_flight(void) { return XvtFlightTask_IsActive(); }

/* Screens inside a mission that only understand mouse and keyboard: the
 * OpenXvT settings overlay. It gets the menu controls, not the gamepad. */
static int vr_flight_menu_active(void) { return XvtSettingsMenu_Open(); }

static float vr_screen_height_m(void) {
	return g_vr.screen_width_m * (float)VR_SCREEN_HEIGHT_PX / (float)VR_SCREEN_WIDTH_PX;
}

/* ------------------------------------------------------------------------ */
/* Event injection                                                          */

static void vr_push_key_mod(SDL_Scancode scancode, int down, SDL_Keymod mod) {
	SDL_Event event;

	SDL_zero(event);
	event.type          = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	event.key.timestamp = SDL_GetTicksNS();
	event.key.scancode  = scancode;
	event.key.key       = SDL_GetKeyFromScancode(scancode, mod, false);
	event.key.mod       = mod;
	event.key.down      = down ? true : false;
	SDL_PushEvent(&event);
}

static void vr_push_key(SDL_Scancode scancode, int down) { vr_push_key_mod(scancode, down, SDL_KMOD_NONE); }

/* Holds `scancode` in `slot` while `pressed`; a slot changing its key while
 * held (mode switch) releases the old key first. */
static void vr_set_key(VrKeySlot slot, SDL_Scancode scancode, int pressed) {
	const SDL_Scancode held = g_vr.held_key[slot];

	if (!pressed || scancode == SDL_SCANCODE_UNKNOWN) {
		if (held != SDL_SCANCODE_UNKNOWN) {
			vr_push_key(held, 0);
			g_vr.held_key[slot] = SDL_SCANCODE_UNKNOWN;
		}
		return;
	}
	if (held == scancode) {
		return;
	}
	if (held != SDL_SCANCODE_UNKNOWN) {
		/* Only a fresh press starts a new key; a held button whose meaning
		 * changed (grip pressed mid-hold) just releases. */
		vr_push_key(held, 0);
		g_vr.held_key[slot] = SDL_SCANCODE_UNKNOWN;
		return;
	}
	vr_push_key(scancode, 1);
	g_vr.held_key[slot] = scancode;
}

static void vr_release_all_keys(void) {
	for (int slot = 0; slot < VR_KEY_SLOT_COUNT; ++slot) {
		vr_set_key((VrKeySlot)slot, SDL_SCANCODE_UNKNOWN, 0);
	}
}

static void vr_push_mouse_button(Uint8 button, int down) {
	SDL_Event event;
	uint64_t  now_ms = SDL_GetTicks();

	SDL_zero(event);
	event.type             = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
	event.button.timestamp = SDL_GetTicksNS();
	event.button.button   = button;
	event.button.down     = down ? true : false;
	event.button.x        = g_vr.pointer_x;
	event.button.y        = g_vr.pointer_y;
	event.button.clicks   = 1;
	if (down && button == SDL_BUTTON_LEFT) {
		const float dx = g_vr.pointer_x - g_vr.last_click_x;
		const float dy = g_vr.pointer_y - g_vr.last_click_y;
		if (now_ms - g_vr.last_click_ms < 450 && dx * dx + dy * dy < 40.0f * 40.0f) {
			event.button.clicks = 2;
			g_vr.last_click_ms  = 0;
		} else {
			g_vr.last_click_ms = now_ms;
		}
		g_vr.last_click_x = g_vr.pointer_x;
		g_vr.last_click_y = g_vr.pointer_y;
	}
	SDL_PushEvent(&event);
}

/* ------------------------------------------------------------------------ */
/* Virtual gamepad                                                          */

static bool SDLCALL vr_pad_rumble(void* userdata, Uint16 low, Uint16 high) {
	(void)userdata;
	g_vr.rumble_low  = low;
	g_vr.rumble_high = high;
	return true;
}

static void vr_create_pad(void) {
	SDL_VirtualJoystickDesc desc;

	if (g_vr.pad_id) {
		return;
	}
	if (!SDL_WasInit(SDL_INIT_JOYSTICK) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
		Aeron_LogError(VR_LOG, "gamepad subsystem unavailable: %s", SDL_GetError());
		return;
	}
	SDL_INIT_INTERFACE(&desc);
	desc.type        = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.naxes       = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons    = SDL_GAMEPAD_BUTTON_MISC1 + 1;
	desc.axis_mask   = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
	desc.button_mask = (1u << (SDL_GAMEPAD_BUTTON_MISC1 + 1)) - 1u;
	desc.name        = "Quest Touch Controllers";
	desc.vendor_id   = VR_PAD_VENDOR_ID;
	desc.product_id  = VR_PAD_PRODUCT_ID;
	desc.Rumble      = vr_pad_rumble;
	g_vr.pad_id      = SDL_AttachVirtualJoystick(&desc);
	if (!g_vr.pad_id) {
		Aeron_LogError(VR_LOG, "SDL_AttachVirtualJoystick failed: %s", SDL_GetError());
		return;
	}
	g_vr.pad = SDL_OpenJoystick(g_vr.pad_id);
	if (!g_vr.pad) {
		Aeron_LogError(VR_LOG, "SDL_OpenJoystick(virtual) failed: %s", SDL_GetError());
		return;
	}
	Aeron_LogInfo(VR_LOG, "Touch controllers attached as virtual gamepad %u", (unsigned)g_vr.pad_id);
}

static Sint16 vr_axis(float value) {
	if (value > 1.0f) {
		value = 1.0f;
	} else if (value < -1.0f) {
		value = -1.0f;
	}
	return (Sint16)(value >= 0.0f ? value * 32767.0f : value * 32768.0f);
}

static Sint16 vr_trigger_axis(float value) {
	if (value < 0.0f) {
		value = 0.0f;
	} else if (value > 1.0f) {
		value = 1.0f;
	}
	return (Sint16)(-32768.0f + value * 65535.0f);
}

/* ------------------------------------------------------------------------ */
/* OpenXR setup                                                             */

static int vr_load_functions(void) {
	p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	if (!p_xrGetInstanceProcAddr) {
		Aeron_LogError(VR_LOG, "xrGetInstanceProcAddr unavailable: %s", SDL_GetError());
		return 0;
	}
#define XR_LOAD(name)                                                                                 \
	if (XR_FAILED(p_xrGetInstanceProcAddr(g_vr.instance, #name, (PFN_xrVoidFunction*)&p_##name)) || \
		!p_##name) {                                                                                  \
		Aeron_LogError(VR_LOG, "missing OpenXR function %s", #name);                                  \
		return 0;                                                                                     \
	}
	XR_FUNCTIONS(XR_LOAD)
#undef XR_LOAD
#define XR_LOAD_OPTIONAL(name) \
	p_xrGetInstanceProcAddr(g_vr.instance, #name, (PFN_xrVoidFunction*)&p_##name);
	XR_LOAD_OPTIONAL(xrGetDisplayRefreshRateFB)
	XR_LOAD_OPTIONAL(xrRequestDisplayRefreshRateFB)
#undef XR_LOAD_OPTIONAL
	return 1;
}

/* Instance extensions requested on top of SDL's graphics-binding ones. */
static const char* g_vr_extensions[4];
static int         g_vr_extension_count;

static void vr_pick_extensions(void) {
	static const char* const wanted[] = {
		XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
		/* PICO 4 / Neo 3 controllers (same button layout as Touch). */
		"XR_BD_controller_interaction",
	};
	PFN_xrGetInstanceProcAddr                  gipa;
	PFN_xrEnumerateInstanceExtensionProperties enumerate = NULL;
	XrExtensionProperties*                     props;
	uint32_t                                   count = 0;

	g_vr_extension_count = 0;
	if (!SDL_OpenXR_LoadLibrary()) {
		Aeron_LogWarn(VR_LOG, "OpenXR loader unavailable: %s", SDL_GetError());
		return;
	}
	gipa = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	if (!gipa || XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
								(PFN_xrVoidFunction*)&enumerate)) ||
		!enumerate || XR_FAILED(enumerate(NULL, 0, &count, NULL)) || count == 0) {
		Aeron_LogWarn(VR_LOG, "cannot enumerate OpenXR extensions");
		return;
	}
	props = (XrExtensionProperties*)SDL_calloc(count, sizeof(*props));
	if (!props) {
		return;
	}
	for (uint32_t i = 0; i < count; ++i) {
		props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}
	if (XR_SUCCEEDED(enumerate(NULL, count, &count, props))) {
		for (size_t w = 0; w < SDL_arraysize(wanted); ++w) {
			for (uint32_t i = 0; i < count; ++i) {
				if (strcmp(props[i].extensionName, wanted[w]) == 0) {
					g_vr_extensions[g_vr_extension_count++] = wanted[w];
					break;
				}
			}
		}
	}
	SDL_free(props);
	for (int i = 0; i < g_vr_extension_count; ++i) {
		Aeron_LogInfo(VR_LOG, "requesting OpenXR extension %s", g_vr_extensions[i]);
		if (strcmp(g_vr_extensions[i], "XR_BD_controller_interaction") == 0) {
			g_vr.has_bd_controllers = 1;
		}
	}
}

static void vr_configure_device(SDL_PropertiesID props, void* userdata) {
	(void)userdata;
	vr_pick_extensions();
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_ENABLE_BOOLEAN, true);
	SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_INSTANCE_POINTER, &g_vr.instance);
	SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_SYSTEM_ID_POINTER, &g_vr.system);
	SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_APPLICATION_NAME_STRING, "OpenXvT VR");
	SDL_SetNumberProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_APPLICATION_VERSION_NUMBER, 1);
	SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_ENGINE_NAME_STRING, "Aeron");
	if (g_vr_extension_count > 0) {
		SDL_SetNumberProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_EXTENSION_COUNT_NUMBER, g_vr_extension_count);
		SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_EXTENSION_NAMES_POINTER,
							   (void*)g_vr_extensions);
	}
}

static int vr_create_actions(void) {
	XrActionSetCreateInfo set_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
	const char* const     profile  = "/interaction_profiles/oculus/touch_controller";

	g_vr.hand_path[VR_LEFT]  = vr_path("/user/hand/left");
	g_vr.hand_path[VR_RIGHT] = vr_path("/user/hand/right");

	SDL_strlcpy(set_info.actionSetName, "xvt", sizeof set_info.actionSetName);
	SDL_strlcpy(set_info.localizedActionSetName, "X-Wing vs. TIE Fighter", sizeof set_info.localizedActionSetName);
	if (!vr_ok(p_xrCreateActionSet(g_vr.instance, &set_info, &g_vr.action_set), "xrCreateActionSet")) {
		return 0;
	}

#define VR_ACTION(field, name, type, both_hands)                                                    \
	do {                                                                                            \
		XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };                                   \
		SDL_strlcpy(info.actionName, name, sizeof info.actionName);                                \
		SDL_strlcpy(info.localizedActionName, name, sizeof info.localizedActionName);              \
		info.actionType          = type;                                                            \
		info.countSubactionPaths = (both_hands) ? 2 : 0;                                            \
		info.subactionPaths      = (both_hands) ? g_vr.hand_path : NULL;                            \
		if (!vr_ok(p_xrCreateAction(g_vr.action_set, &info, &g_vr.field), "xrCreateAction " name)) { \
			return 0;                                                                               \
		}                                                                                           \
	} while (0)
	VR_ACTION(act_aim, "aim", XR_ACTION_TYPE_POSE_INPUT, 1);
	VR_ACTION(act_trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, 1);
	VR_ACTION(act_grip, "grip", XR_ACTION_TYPE_FLOAT_INPUT, 1);
	VR_ACTION(act_stick, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT, 1);
	VR_ACTION(act_stick_click, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, 1);
	VR_ACTION(act_haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, 1);
	VR_ACTION(act_a, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
	VR_ACTION(act_b, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
	VR_ACTION(act_x, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
	VR_ACTION(act_y, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
	VR_ACTION(act_menu, "button_menu", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
#undef VR_ACTION

	{
		const XrActionSuggestedBinding bindings[] = {
			{ g_vr.act_aim, vr_path("/user/hand/left/input/aim/pose") },
			{ g_vr.act_aim, vr_path("/user/hand/right/input/aim/pose") },
			{ g_vr.act_trigger, vr_path("/user/hand/left/input/trigger/value") },
			{ g_vr.act_trigger, vr_path("/user/hand/right/input/trigger/value") },
			{ g_vr.act_grip, vr_path("/user/hand/left/input/squeeze/value") },
			{ g_vr.act_grip, vr_path("/user/hand/right/input/squeeze/value") },
			{ g_vr.act_stick, vr_path("/user/hand/left/input/thumbstick") },
			{ g_vr.act_stick, vr_path("/user/hand/right/input/thumbstick") },
			{ g_vr.act_stick_click, vr_path("/user/hand/left/input/thumbstick/click") },
			{ g_vr.act_stick_click, vr_path("/user/hand/right/input/thumbstick/click") },
			{ g_vr.act_haptic, vr_path("/user/hand/left/output/haptic") },
			{ g_vr.act_haptic, vr_path("/user/hand/right/output/haptic") },
			{ g_vr.act_a, vr_path("/user/hand/right/input/a/click") },
			{ g_vr.act_b, vr_path("/user/hand/right/input/b/click") },
			{ g_vr.act_x, vr_path("/user/hand/left/input/x/click") },
			{ g_vr.act_y, vr_path("/user/hand/left/input/y/click") },
			{ g_vr.act_menu, vr_path("/user/hand/left/input/menu/click") },
		};
		XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		suggested.interactionProfile     = vr_path(profile);
		suggested.countSuggestedBindings = (uint32_t)SDL_arraysize(bindings);
		suggested.suggestedBindings      = bindings;
		if (!vr_ok(p_xrSuggestInteractionProfileBindings(g_vr.instance, &suggested),
				   "xrSuggestInteractionProfileBindings")) {
			return 0;
		}
		/* PICO: same paths on the ByteDance profiles; optional. */
		if (g_vr.has_bd_controllers) {
			static const char* const pico_profiles[] = {
				"/interaction_profiles/bytedance/pico4_controller",
				"/interaction_profiles/bytedance/pico_neo3_controller",
			};
			for (size_t i = 0; i < SDL_arraysize(pico_profiles); ++i) {
				suggested.interactionProfile = vr_path(pico_profiles[i]);
				if (XR_SUCCEEDED(p_xrSuggestInteractionProfileBindings(g_vr.instance, &suggested))) {
					Aeron_LogInfo(VR_LOG, "controller bindings for %s", pico_profiles[i]);
				} else {
					Aeron_LogWarn(VR_LOG, "no controller bindings for %s", pico_profiles[i]);
				}
			}
		}
	}

	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		XrActionSpaceCreateInfo info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		info.action                  = g_vr.act_aim;
		info.subactionPath           = g_vr.hand_path[hand];
		info.poseInActionSpace.orientation.w = 1.0f;
		if (!vr_ok(p_xrCreateActionSpace(g_vr.session, &info, &g_vr.aim_space[hand]), "xrCreateActionSpace")) {
			return 0;
		}
	}

	{
		XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		attach.countActionSets               = 1;
		attach.actionSets                    = &g_vr.action_set;
		if (!vr_ok(p_xrAttachSessionActionSets(g_vr.session, &attach), "xrAttachSessionActionSets")) {
			return 0;
		}
	}
	return 1;
}

static SDL_GPUTextureFormat vr_pick_format(int* out_ok) {
	static const SDL_GPUTextureFormat preferred[] = {
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
		SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
	};
	int                   count   = 0;
	SDL_GPUTextureFormat* formats = SDL_GetGPUXRSwapchainFormats(g_vr.device, g_vr.session, &count);
	SDL_GPUTextureFormat  chosen  = SDL_GPU_TEXTUREFORMAT_INVALID;

	*out_ok = 0;
	if (!formats || count <= 0) {
		Aeron_LogError(VR_LOG, "no OpenXR swapchain formats: %s", SDL_GetError());
		SDL_free(formats);
		return chosen;
	}
	for (size_t p = 0; p < SDL_arraysize(preferred) && chosen == SDL_GPU_TEXTUREFORMAT_INVALID; ++p) {
		for (int i = 0; i < count; ++i) {
			if (formats[i] == preferred[p]) {
				chosen = preferred[p];
				break;
			}
		}
	}
	SDL_free(formats);
	*out_ok = chosen != SDL_GPU_TEXTUREFORMAT_INVALID;
	return chosen;
}

static void vr_create_keyboard(SDL_GPUTextureFormat format, const XrSwapchainCreateInfo* base);
static void vr_create_radials(SDL_GPUTextureFormat format, const XrSwapchainCreateInfo* base);

static int vr_device_created(SDL_GPUDevice* device, AeronExternalPresentTarget* target, void* userdata) {
	XrSessionCreateInfo        session_info = { XR_TYPE_SESSION_CREATE_INFO };
	XrReferenceSpaceCreateInfo space_info   = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	XrSwapchainCreateInfo      swap_info    = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	SDL_GPUTextureFormat       format;
	int                        format_ok;

	(void)userdata;
	g_vr.device = device;
	if (!vr_load_functions()) {
		return 0;
	}
	if (!vr_ok(SDL_CreateGPUXRSession(device, &session_info, &g_vr.session), "SDL_CreateGPUXRSession")) {
		return 0;
	}

	space_info.referenceSpaceType           = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	if (!vr_ok(p_xrCreateReferenceSpace(g_vr.session, &space_info, &g_vr.local_space), "xrCreateReferenceSpace")) {
		return 0;
	}

	format = vr_pick_format(&format_ok);
	if (!format_ok) {
		return 0;
	}
	swap_info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	swap_info.sampleCount = 1;
	swap_info.width       = VR_SCREEN_WIDTH_PX;
	swap_info.height      = VR_SCREEN_HEIGHT_PX;
	swap_info.faceCount   = 1;
	swap_info.arraySize   = 1;
	swap_info.mipCount    = 1;
	if (!vr_ok(SDL_CreateGPUXRSwapchain(device, g_vr.session, &swap_info, format, &g_vr.swapchain, &g_vr.images),
			   "SDL_CreateGPUXRSwapchain")) {
		return 0;
	}
	vr_create_keyboard(format, &swap_info);
	vr_create_radials(format, &swap_info);

	{
		XrViewConfigurationView config[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW },
											  { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
		uint32_t                count     = 0;
		if (vr_ok(p_xrEnumerateViewConfigurationViews(g_vr.instance, g_vr.system,
													   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, config),
				  "xrEnumerateViewConfigurationViews") &&
			count == 2) {
			g_vr.eye_width  = ((int)((float)config[0].recommendedImageRectWidth * VR_EYE_SCALE) + 7) & ~7;
			g_vr.eye_height = ((int)((float)config[0].recommendedImageRectHeight * VR_EYE_SCALE) + 7) & ~7;
			for (int eye = 0; eye < 2; ++eye) {
				XrSwapchainCreateInfo eye_info = swap_info;
				eye_info.width                 = (uint32_t)g_vr.eye_width;
				eye_info.height                = (uint32_t)g_vr.eye_height;
				if (!vr_ok(SDL_CreateGPUXRSwapchain(device, g_vr.session, &eye_info, format, &g_vr.eye_swapchain[eye],
													&g_vr.eye_images[eye]),
						   "SDL_CreateGPUXRSwapchain(eye)")) {
					g_vr.eye_width = g_vr.eye_height = 0;
					break;
				}
			}
			Aeron_LogInfo(VR_LOG, "stereo eyes %dx%d (recommended %ux%u)", g_vr.eye_width, g_vr.eye_height,
						  config[0].recommendedImageRectWidth, config[0].recommendedImageRectHeight);
		}
	}

	if (!vr_create_actions()) {
		return 0;
	}

	g_vr.refresh_hz = 72.0f;
	/* Pin the panel rate: an unpinned rate drifts and the frame pacing
	 * (xrWaitFrame) then produces stale frames. */
	if (p_xrRequestDisplayRefreshRateFB &&
		XR_SUCCEEDED(p_xrRequestDisplayRefreshRateFB(g_vr.session, VR_REFRESH_HZ))) {
		g_vr.refresh_hz = VR_REFRESH_HZ;
	}
	if (g_vr.refresh_hz != VR_REFRESH_HZ && p_xrGetDisplayRefreshRateFB) {
		float hz = 0.0f;
		if (XR_SUCCEEDED(p_xrGetDisplayRefreshRateFB(g_vr.session, &hz)) && hz > 1.0f) {
			g_vr.refresh_hz = hz;
		}
	}
	g_vr.screen_width_m = VR_SCREEN_WIDTH_MENU;
	g_vr.pointer_hand   = VR_LEFT;
	vr_create_pad();

	target->format     = format;
	target->width      = VR_SCREEN_WIDTH_PX;
	target->height     = VR_SCREEN_HEIGHT_PX;
	target->refresh_hz = g_vr.refresh_hz;
	Aeron_LogInfo(VR_LOG, "OpenXR session ready: screen %dx%d, format %d, %.0f Hz", VR_SCREEN_WIDTH_PX,
				  VR_SCREEN_HEIGHT_PX, (int)format, (double)g_vr.refresh_hz);
	return 1;
}

/* ------------------------------------------------------------------------ */
/* Session lifecycle                                                        */

static void vr_request_quit(void) {
	SDL_Event event;

	if (g_vr.quit_pushed) {
		return;
	}
	g_vr.quit_pushed = 1;
	SDL_zero(event);
	event.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&event);
}

static void vr_poll_events(void) {
	XrEventDataBuffer event;

	for (;;) {
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
		event.next = NULL;
		if (p_xrPollEvent(g_vr.instance, &event) != XR_SUCCESS) {
			break;
		}
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const XrEventDataSessionStateChanged* changed = (const XrEventDataSessionStateChanged*)&event;
			g_vr.state                                    = changed->state;
			Aeron_LogInfo(VR_LOG, "session state %d", (int)changed->state);
			switch (changed->state) {
				case XR_SESSION_STATE_READY: {
					XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
					begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (vr_ok(p_xrBeginSession(g_vr.session, &begin), "xrBeginSession")) {
						g_vr.running = 1;
					}
					break;
				}
				case XR_SESSION_STATE_STOPPING:
					p_xrEndSession(g_vr.session);
					g_vr.running = 0;
					break;
				case XR_SESSION_STATE_EXITING:
				case XR_SESSION_STATE_LOSS_PENDING:
					g_vr.running = 0;
					vr_request_quit();
					break;
				default:
					break;
			}
			g_vr.focused = changed->state == XR_SESSION_STATE_FOCUSED;
			Aeron_SetExternalFocus(g_vr.focused);
			if (!g_vr.focused) {
				vr_release_all_keys();
			}
		} else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g_vr.running = 0;
			vr_request_quit();
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */

static float vr_get_float(XrAction action, VrHand hand) {
	XrActionStateGetInfo info  = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateFloat   state = { XR_TYPE_ACTION_STATE_FLOAT };
	info.action                = action;
	info.subactionPath         = g_vr.hand_path[hand];
	return XR_SUCCEEDED(p_xrGetActionStateFloat(g_vr.session, &info, &state)) && state.isActive
			   ? state.currentState
			   : 0.0f;
}

static int vr_get_bool(XrAction action, XrPath subaction) {
	XrActionStateGetInfo info  = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
	info.action                = action;
	info.subactionPath         = subaction;
	return XR_SUCCEEDED(p_xrGetActionStateBoolean(g_vr.session, &info, &state)) && state.isActive &&
		   state.currentState;
}

static XrVector2f vr_get_stick(VrHand hand) {
	XrActionStateGetInfo  info   = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateVector2f state  = { XR_TYPE_ACTION_STATE_VECTOR2F };
	XrVector2f            result = { 0.0f, 0.0f };
	info.action                  = g_vr.act_stick;
	info.subactionPath           = g_vr.hand_path[hand];
	if (XR_SUCCEEDED(p_xrGetActionStateVector2f(g_vr.session, &info, &state)) && state.isActive) {
		result = state.currentState;
	}
	return result;
}

static int vr_hysteresis(int was_down, float value) { return was_down ? value > 0.35f : value > 0.65f; }

/* Controller aim ray in LOCAL space. */
static int vr_locate_aim(VrHand hand, XrVector3f* origin, XrVector3f* dir) {
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };

	if (XR_FAILED(p_xrLocateSpace(g_vr.aim_space[hand], g_vr.local_space, g_vr.frame_state.predictedDisplayTime,
								  &location)) ||
		!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
		!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		return 0;
	}
	*origin = location.pose.position;
	*dir    = vr_rotate(location.pose.orientation, (XrVector3f) { 0.0f, 0.0f, -1.0f });
	return 1;
}

/* Ray from a controller onto the virtual screen; returns screen pixels. */
static int vr_pointer_hit(VrHand hand, float* out_x, float* out_y) {
	const float      width   = g_vr.screen_width_m;
	const float      height  = vr_screen_height_m();
	XrVector3f       dir;
	XrVector3f       origin;
	float            t, hit_x, hit_y, u, v;

	if (!vr_locate_aim(hand, &origin, &dir)) {
		return 0;
	}
	if (dir.z > -1e-4f) {
		return 0;
	}
	t = (-VR_SCREEN_DISTANCE - origin.z) / dir.z;
	if (t <= 0.0f) {
		return 0;
	}
	hit_x = origin.x + dir.x * t;
	hit_y = origin.y + dir.y * t;
	u     = hit_x / width + 0.5f;
	v     = 0.5f - hit_y / height;
	if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) {
		return 0;
	}
	*out_x = u * (float)VR_SCREEN_WIDTH_PX;
	*out_y = v * (float)VR_SCREEN_HEIGHT_PX;
	return 1;
}

static void vr_update_pointer(void) {
	float x, y;

	/* No laser in the OpenXvT settings overlay (gamepad only there). In the
	 * main menus either controller is the mouse (the hand that last pulled
	 * its trigger); in a mission only the left one. */
	if (vr_in_flight()) {
		g_vr.pointer_hand = VR_LEFT;
	} else {
		for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
			if (g_vr.trigger[hand] > 0.65f && !g_vr.trigger_down[hand]) {
				g_vr.pointer_hand = (VrHand)hand;
			}
		}
	}
	/* A ray on the virtual keyboard doesn't also point at the screen. */
	g_vr.pointer_valid = !XvtSettingsMenu_Open() && !g_vr.kb_hit[g_vr.pointer_hand] &&
						 vr_pointer_hit(g_vr.pointer_hand, &x, &y);
	if (g_vr.pointer_valid) {
		g_vr.pointer_x = x;
		g_vr.pointer_y = y;
	}
	Aeron_SetExternalPointer(g_vr.pointer_valid, g_vr.pointer_x, g_vr.pointer_y);
}

static void vr_update_mouse_buttons(int in_flight) {
	const VrHand hand       = g_vr.pointer_hand;
	const int    laser      = !in_flight && !XvtSettingsMenu_Open();
	const int    left_down  = laser && vr_hysteresis(g_vr.mouse_left_down, g_vr.trigger[hand]);
	/* In game menus the laser hand's grip right-clicks (in flight the grips
	 * open the radial menus). */
	const int    right_down = laser && vr_hysteresis(g_vr.mouse_right_down, g_vr.grip[hand]);

	if (left_down != g_vr.mouse_left_down) {
		if (!left_down || g_vr.pointer_valid) {
			vr_push_mouse_button(SDL_BUTTON_LEFT, left_down);
			g_vr.mouse_left_down = left_down;
		}
	}
	if (right_down != g_vr.mouse_right_down) {
		if (!right_down || g_vr.pointer_valid) {
			vr_push_mouse_button(SDL_BUTTON_RIGHT, right_down);
			g_vr.mouse_right_down = right_down;
		}
	}
}

/* The Touch controllers as one gamepad, so every flight input can be bound
 * in the game's own controller options (defaults in config.yaml):
 *   physical left stick -> right x/y (pitch/yaw), physical right stick x ->
 *   left x (roll), triggers -> trigger axes, A/B/X/Y -> south/east/west/north,
 *   physical left/right stick click -> right/left stick button, menu -> start.
 * Physical right stick y is a throttle lever: pushing it moves the lever,
 * releasing it leaves the lever where it is (the game applies the throttle
 * axis only when it changes, so a spring-centred stick would keep snapping to
 * half speed). The grips open the radial menus (vr_update_radials); a stick
 * choosing in its menu sends nothing to the pad. */
static void vr_update_gamepad(int in_flight, float dt) {
	/* Sticks swapped: physical right stick = roll + throttle lever, physical
	 * left stick = pitch/yaw; the stick clicks swap with them. */
	const XrVector2f left  = g_vr.stick[VR_RIGHT];
	const XrVector2f right = g_vr.stick[VR_LEFT];
	float            roll;
	float            lever_input;
	float            trigger[VR_HAND_COUNT];

	if (!g_vr.pad) {
		return;
	}
	/* The dominant left-stick direction wins so moving the lever doesn't roll. */
	roll        = SDL_fabsf(left.x) > SDL_fabsf(left.y) * 0.75f ? left.x : 0.0f;
	lever_input = SDL_fabsf(left.y) > SDL_fabsf(left.x) * 0.75f ? left.y : 0.0f;
	/* A stick that is choosing in its radial menu doesn't fly. */
	if (g_vr.radial[VR_RIGHT].visible) {
		roll        = 0.0f;
		lever_input = 0.0f;
	}
	if (in_flight && SDL_fabsf(lever_input) > 0.2f) {
		g_vr.throttle += lever_input * 0.6f * dt;
		if (g_vr.throttle < 0.0f) {
			g_vr.throttle = 0.0f;
		} else if (g_vr.throttle > 1.0f) {
			g_vr.throttle = 1.0f;
		}
	}
	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		trigger[hand] = g_vr.trigger[hand];
	}

	SDL_LockJoysticks();
	if (!in_flight) {
		/* Menus. The throttle lever is never reported here: it stays in
		 * g_vr.throttle and comes back (as a change) when flight resumes
		 * (reporting it made the settings list scroll forever). Game menus get
		 * a neutral pad (the laser is the pointer). The OpenXvT settings
		 * overlay is gamepad only, left controller: stick = focus, X = select
		 * (south), Y = back (east), left trigger / grip = previous / next page
		 * (shoulders). */
		const int        settings   = XvtSettingsMenu_Open();
		const XrVector2f menu_stick = g_vr.stick[VR_LEFT];
		for (int b = 0; b <= SDL_GAMEPAD_BUTTON_MISC1; ++b) {
			SDL_SetJoystickVirtualButton(g_vr.pad, b, 0);
		}
		SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_SOUTH, settings && g_vr.button_x);
		SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_EAST, settings && g_vr.button_y);
		SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
									 settings && vr_hysteresis(g_vr.trigger_down[VR_LEFT], g_vr.trigger[VR_LEFT]));
		SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
									 settings && vr_hysteresis(g_vr.grip_down[VR_LEFT], g_vr.grip[VR_LEFT]));
		SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTX, 0);
		SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTY, 0);
		SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTX, settings ? vr_axis(menu_stick.x) : 0);
		SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTY, settings ? vr_axis(-menu_stick.y) : 0);
		SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, vr_trigger_axis(0.0f));
		SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, vr_trigger_axis(0.0f));
		SDL_UnlockJoysticks();
		return;
	}
	SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTX,
							   g_vr.radial[VR_LEFT].visible ? 0 : vr_axis(right.x));
	SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTY,
							   g_vr.radial[VR_LEFT].visible ? 0 : vr_axis(right.y)); /* inverted pitch: stick up = SDL down = nose down */
	SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTX, vr_axis(roll));
	SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFTY, vr_axis(g_vr.throttle * 2.0f - 1.0f));
	SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, vr_trigger_axis(trigger[VR_LEFT]));
	SDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, vr_trigger_axis(trigger[VR_RIGHT]));
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_SOUTH, g_vr.button_a);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_EAST, g_vr.button_b);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_WEST, g_vr.button_x);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_NORTH, g_vr.button_y);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_LEFT_STICK, g_vr.stick_click[VR_RIGHT]);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK, g_vr.stick_click[VR_LEFT]);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_START, g_vr.button_menu);
	/* Unused since the radial menus replaced the left-grip shift layer. */
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_MISC1, 0);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_GUIDE, 0);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_BACK, 0);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_UP, 0);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0);
	SDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0);
	SDL_UnlockJoysticks();
}

/* Menus (concourse, briefing, options, and in a mission the settings
 * overlay) have no gamepad navigation, so there the buttons
 * type keys (left controller): X = Enter, Y = Space, menu = Escape,
 * left stick = arrows; the OpenXvT settings overlay gets the gamepad. While
 * flying nothing is typed; see vr_update_gamepad. */
static void vr_update_menu_keys(int in_flight) {
	const int   menu = !in_flight && !XvtSettingsMenu_Open(); /* settings overlay: pad only */
	const float lx   = g_vr.stick[VR_LEFT].x;
	const float ly   = g_vr.stick[VR_LEFT].y;

	vr_set_key(VR_KEY_A, SDL_SCANCODE_UNKNOWN, 0);
	vr_set_key(VR_KEY_B, SDL_SCANCODE_UNKNOWN, 0);
	vr_set_key(VR_KEY_X, SDL_SCANCODE_RETURN, menu && g_vr.button_x);
	vr_set_key(VR_KEY_Y, SDL_SCANCODE_SPACE, menu && g_vr.button_y);
	vr_set_key(VR_KEY_RSTICK_CLICK, SDL_SCANCODE_UNKNOWN, 0);
	vr_set_key(VR_KEY_LSTICK_CLICK, SDL_SCANCODE_UNKNOWN, 0);
	vr_set_key(VR_KEY_LTRIGGER, SDL_SCANCODE_UNKNOWN, 0);
	vr_set_key(VR_KEY_MENU, SDL_SCANCODE_ESCAPE, !in_flight && g_vr.button_menu);
	vr_set_key(VR_KEY_UP, SDL_SCANCODE_UP, menu && ly > (g_vr.held_key[VR_KEY_UP] ? 0.4f : 0.7f));
	vr_set_key(VR_KEY_DOWN, SDL_SCANCODE_DOWN, menu && ly < -(g_vr.held_key[VR_KEY_DOWN] ? 0.4f : 0.7f));
	vr_set_key(VR_KEY_LEFT, SDL_SCANCODE_LEFT, menu && lx < -(g_vr.held_key[VR_KEY_LEFT] ? 0.4f : 0.7f));
	vr_set_key(VR_KEY_RIGHT, SDL_SCANCODE_RIGHT, menu && lx > (g_vr.held_key[VR_KEY_RIGHT] ? 0.4f : 0.7f));
}

/* ------------------------------------------------------------------------ */
/* Virtual keyboard                                                         */

static XrQuaternionf vr_kb_orientation(void) {
	const float half = VR_KB_TILT_DEG * SDL_PI_F / 360.0f;
	return (XrQuaternionf) { sinf(half), 0.0f, 0.0f, cosf(half) };
}

static float vr_kb_height_m(void) { return VR_KB_WIDTH_M * (float)VR_KB_HEIGHT_PX / (float)VR_KB_WIDTH_PX; }

/* Ray from a controller onto the keyboard panel; returns 0..1 image coords. */
static int vr_kb_hit(VrHand hand, float* out_u, float* out_v) {
	XrQuaternionf inverse = vr_kb_orientation();
	XrVector3f    origin, dir;
	float         t, u, v;

	if (!vr_locate_aim(hand, &origin, &dir)) {
		return 0;
	}
	inverse.x = -inverse.x;
	origin.y -= VR_KB_HEIGHT;
	origin.z += VR_KB_DISTANCE;
	origin = vr_rotate(inverse, origin);
	dir    = vr_rotate(inverse, dir);
	if (dir.z > -1e-4f) {
		return 0;
	}
	t = -origin.z / dir.z;
	if (t <= 0.0f) {
		return 0;
	}
	u = (origin.x + dir.x * t) / VR_KB_WIDTH_M + 0.5f;
	v = 0.5f - (origin.y + dir.y * t) / vr_kb_height_m();
	if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) {
		return 0;
	}
	*out_u = u;
	*out_v = v;
	return 1;
}

static void vr_kb_haptic(VrHand hand) {
	XrHapticActionInfo info      = { XR_TYPE_HAPTIC_ACTION_INFO };
	XrHapticVibration  vibration = { XR_TYPE_HAPTIC_VIBRATION };

	info.action         = g_vr.act_haptic;
	info.subactionPath  = g_vr.hand_path[hand];
	vibration.amplitude = 0.4f;
	vibration.duration  = 15000000; /* 15 ms */
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	p_xrApplyHapticFeedback(g_vr.session, &info, (const XrHapticBaseHeader*)&vibration);
}

static void vr_kb_type(int key, uint64_t now_ms) {
	const VrKeyKind kind = VrKeyboard_KeyKind(key);
	const char*     text = VrKeyboard_Press(&g_vr.kb, key, now_ms);

	g_vr.kb_dirty = 1;
	if (kind == VR_KEY_KIND_BACKSPACE || kind == VR_KEY_KIND_ENTER) {
		const SDL_Scancode scancode = kind == VR_KEY_KIND_BACKSPACE ? SDL_SCANCODE_BACKSPACE : SDL_SCANCODE_RETURN;
		vr_push_key(scancode, 1);
		vr_push_key(scancode, 0);
	} else if (text) {
		SDL_Event event;
		SDL_zero(event);
		event.type           = SDL_EVENT_TEXT_INPUT;
		event.text.timestamp = SDL_GetTicksNS();
		event.text.text      = text; /* static string; SDL copies it anyway */
		SDL_PushEvent(&event);
	}
}

/* Shows the keyboard while a game text field takes input (outside flight),
 * tracks both controller rays on it and types on trigger presses. Backspace
 * repeats while held. Runs before vr_update_pointer, which leaves the screen
 * cursor alone while the pointer hand aims at the keyboard. */
static void vr_update_keyboard(int flying) {
	const uint64_t now_ms = SDL_GetTicks();
	int            visible;

	if (g_textFieldEditSerial != g_vr.kb_serial) {
		g_vr.kb_serial    = g_textFieldEditSerial;
		g_vr.kb_active_ms = now_ms;
	}
	visible = g_vr.kb_swapchain && !flying && g_vr.kb_active_ms && now_ms - g_vr.kb_active_ms < VR_KB_LINGER_MS;
	if (visible && !g_vr.kb_visible) {
		VrKeyboard_Reset(&g_vr.kb);
		g_vr.kb_dirty = 1;
		Aeron_LogInfo(VR_LOG, "virtual keyboard shown");
	}
	g_vr.kb_visible = visible;

	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		const int was_down = g_vr.trigger_down[hand];
		const int down     = vr_hysteresis(was_down, g_vr.trigger[hand]);
		int       key      = VR_KB_NO_KEY;
		float     u, v;

		g_vr.kb_hit[hand] = visible && vr_kb_hit((VrHand)hand, &u, &v);
		if (g_vr.kb_hit[hand]) {
			key = VrKeyboard_KeyAt(u, v);
		}
		if (key != g_vr.kb.hover[hand]) {
			g_vr.kb.hover[hand] = key;
			g_vr.kb_dirty       = 1;
		}
		if (!down || key == VR_KB_NO_KEY) {
			g_vr.kb_held_key[hand] = VR_KB_NO_KEY;
			continue;
		}
		if (!was_down) {
			vr_kb_type(key, now_ms);
			vr_kb_haptic((VrHand)hand);
			g_vr.kb_held_key[hand]  = key;
			g_vr.kb_repeat_ms[hand] = now_ms + 450;
		} else if (key == g_vr.kb_held_key[hand] && VrKeyboard_KeyKind(key) == VR_KEY_KIND_BACKSPACE &&
				   now_ms >= g_vr.kb_repeat_ms[hand]) {
			vr_kb_type(key, now_ms);
			g_vr.kb_repeat_ms[hand] = now_ms + 70;
		}
	}
	if (g_vr.kb_drawn_flash && now_ms >= g_vr.kb.flash_until_ms) {
		g_vr.kb_dirty = 1;
	}
}

/* Panel pose for a radial menu: above the controller, turned to the head. */
static int vr_radial_pose(VrHand hand, XrPosef* pose) {
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	XrVector3f      to_head;
	float           len, yaw, pitch;

	if (XR_FAILED(p_xrLocateSpace(g_vr.aim_space[hand], g_vr.local_space, g_vr.frame_state.predictedDisplayTime,
								  &location)) ||
		!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
		return 0;
	}
	pose->position = location.pose.position;
	pose->position.y += VR_RADIAL_LIFT_M;
	to_head.x = g_vr.head_pos.x - pose->position.x;
	to_head.y = g_vr.head_pos.y - pose->position.y;
	to_head.z = g_vr.head_pos.z - pose->position.z;
	len       = sqrtf(to_head.x * to_head.x + to_head.y * to_head.y + to_head.z * to_head.z);
	if (len < 1e-3f) {
		pose->orientation = (XrQuaternionf) { 0.0f, 0.0f, 0.0f, 1.0f };
		return 1;
	}
	/* Quad normal (+z) towards the head: yaw about y, then pitch about x. */
	yaw   = atan2f(to_head.x, to_head.z);
	pitch = -asinf(to_head.y / len);
	{
		const float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f);
		const float sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
		pose->orientation = (XrQuaternionf) { cy * sp, sy * cp, -sy * sp, cy * cp };
	}
	return 1;
}

static void vr_run_command(const VrCommand* command) {
	XvtControllerMapping_DispatchCommand((XvtInputAction)command->action, true);
	XvtControllerMapping_DispatchCommand((XvtInputAction)command->action, false);
	Aeron_LogInfo(VR_LOG, "radial menu: %s", command->label);
}

/* Radial menus in flight. Left grip held: weapons/power, right grip held:
 * targets/orders. The same hand's stick picks an entry; releasing the grip
 * runs it (stick centred = cancel). */
static void vr_update_radials(int flying) {
	const uint64_t now_ms = SDL_GetTicks();

	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		VrRadialState*   radial = &g_vr.radial[hand];
		const int        held   = flying && vr_hysteresis(g_vr.grip_down[hand], g_vr.grip[hand]);
		const XrVector2f stick  = g_vr.stick[hand];
		const float      mag    = sqrtf(stick.x * stick.x + stick.y * stick.y);
		int              open   = held;

		if (!radial->panel.swapchain) {
			open = 0;
		}

		if (open && !radial->visible) {
			radial->selected    = VR_RADIAL_NONE;
			radial->suppressed  = 0;
			radial->panel.dirty = 1;
		}
		if (open) {
			const int item = !radial->suppressed &&
									 mag > (radial->selected != VR_RADIAL_NONE ? VR_RADIAL_KEEP : VR_RADIAL_PICK)
								 ? VrRadialMenu_ItemForDirection(stick.x, stick.y)
								 : VR_RADIAL_NONE;
			if (item != VR_RADIAL_NONE) {
				if (item != radial->selected) {
					radial->selected = item;
					vr_kb_haptic((VrHand)hand);
				}
				radial->selected_ms = now_ms;
			} else if (radial->selected != VR_RADIAL_NONE && now_ms - radial->selected_ms > VR_RADIAL_LINGER_MS) {
				radial->selected = VR_RADIAL_NONE;
			}
			if (radial->selected != radial->drawn_selected) {
				radial->panel.dirty = 1;
			}
			radial->pose_valid = vr_radial_pose((VrHand)hand, &radial->pose);
		} else if (radial->visible && !held && radial->selected != VR_RADIAL_NONE) {
			/* Grip released with an entry chosen: run it. */
			vr_run_command(VrRadialMenu_Item((VrRadialSide)hand, radial->selected));
			vr_kb_haptic((VrHand)hand);
		}
		radial->visible = open;
	}
}

/* Uploads a CPU-drawn panel image into its swapchain. */
static void vr_panel_upload(VrPanel* panel) {
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo    wait_info    = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	uint32_t                    index        = 0;
	SDL_GPUCommandBuffer*       command_buffer;
	void*                       mapped;

	mapped = SDL_MapGPUTransferBuffer(g_vr.device, panel->transfer, true);
	if (!mapped) {
		return;
	}
	memcpy(mapped, panel->pixels, (size_t)panel->width * panel->height * 4);
	SDL_UnmapGPUTransferBuffer(g_vr.device, panel->transfer);
	if (!vr_ok(p_xrAcquireSwapchainImage(panel->swapchain, &acquire_info, &index), "xrAcquireSwapchainImage(panel)")) {
		return;
	}
	wait_info.timeout = XR_INFINITE_DURATION;
	if (!vr_ok(p_xrWaitSwapchainImage(panel->swapchain, &wait_info), "xrWaitSwapchainImage(panel)")) {
		p_xrReleaseSwapchainImage(panel->swapchain, &release_info);
		return;
	}
	command_buffer = SDL_AcquireGPUCommandBuffer(g_vr.device);
	if (command_buffer) {
		SDL_GPUTextureTransferInfo source;
		SDL_GPUTextureRegion       region;
		SDL_GPUCopyPass*           copy_pass;
		SDL_zero(source);
		SDL_zero(region);
		source.transfer_buffer = panel->transfer;
		source.pixels_per_row  = (Uint32)panel->width;
		source.rows_per_layer  = (Uint32)panel->height;
		region.texture         = panel->images[index];
		region.w               = (Uint32)panel->width;
		region.h               = (Uint32)panel->height;
		region.d               = 1;
		copy_pass              = SDL_BeginGPUCopyPass(command_buffer);
		SDL_UploadToGPUTexture(copy_pass, &source, &region, false);
		SDL_EndGPUCopyPass(copy_pass);
		if (SDL_SubmitGPUCommandBuffer(command_buffer)) {
			panel->dirty = 0;
		} else {
			Aeron_LogError(VR_LOG, "panel upload failed: %s", SDL_GetError());
		}
	}
	p_xrReleaseSwapchainImage(panel->swapchain, &release_info);
	panel->ever_released = 1;
}

static void vr_upload_radials(void) {
	if (!g_vr.frame_begun) {
		return;
	}
	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		VrRadialState* radial = &g_vr.radial[hand];
		if (!radial->visible || !radial->panel.dirty) {
			continue;
		}
		VrRadialMenu_Render((VrRadialSide)hand, radial->selected, radial->panel.pixels, g_vr.panel_bgra);
		radial->drawn_selected = radial->selected;
		vr_panel_upload(&radial->panel);
	}
}

static int vr_panel_create(VrPanel* panel, SDL_GPUTextureFormat format, const XrSwapchainCreateInfo* base, int width,
						   int height) {
	XrSwapchainCreateInfo           info     = *base;
	SDL_GPUTransferBufferCreateInfo transfer = { 0 };

	SDL_zero(*panel);
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
					  XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	info.width  = (uint32_t)width;
	info.height = (uint32_t)height;
	if (!vr_ok(SDL_CreateGPUXRSwapchain(g_vr.device, g_vr.session, &info, format, &panel->swapchain, &panel->images),
			   "SDL_CreateGPUXRSwapchain(panel)")) {
		SDL_zero(*panel);
		return 0;
	}
	transfer.usage  = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer.size   = (Uint32)width * (Uint32)height * 4;
	panel->transfer = SDL_CreateGPUTransferBuffer(g_vr.device, &transfer);
	panel->pixels   = SDL_malloc(transfer.size);
	panel->width    = width;
	panel->height   = height;
	if (!panel->transfer || !panel->pixels) {
		Aeron_LogError(VR_LOG, "panel unavailable: %s", SDL_GetError());
		SDL_DestroyGPUXRSwapchain(g_vr.device, panel->swapchain, panel->images);
		if (panel->transfer) {
			SDL_ReleaseGPUTransferBuffer(g_vr.device, panel->transfer);
		}
		SDL_free(panel->pixels);
		SDL_zero(*panel);
		return 0;
	}
	return 1;
}

static void vr_panel_destroy(SDL_GPUDevice* device, VrPanel* panel) {
	if (panel->swapchain) {
		SDL_DestroyGPUXRSwapchain(device, panel->swapchain, panel->images);
	}
	if (panel->transfer) {
		SDL_ReleaseGPUTransferBuffer(device, panel->transfer);
	}
	SDL_free(panel->pixels);
	SDL_zero(*panel);
}

static void vr_create_radials(SDL_GPUTextureFormat format, const XrSwapchainCreateInfo* base) {
	g_vr.panel_bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		g_vr.radial[hand].selected       = VR_RADIAL_NONE;
		g_vr.radial[hand].drawn_selected = VR_RADIAL_NONE;
		if (!vr_panel_create(&g_vr.radial[hand].panel, format, base, VR_RADIAL_SIZE_PX, VR_RADIAL_SIZE_PX)) {
			Aeron_LogError(VR_LOG, "radial menu %d unavailable", hand);
		}
	}
	Aeron_LogInfo(VR_LOG, "radial menus ready (%dx%d)", VR_RADIAL_SIZE_PX, VR_RADIAL_SIZE_PX);
}

/* Redraws the keyboard image into its swapchain when it changed. */
static void vr_upload_keyboard(void) {
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo    wait_info    = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	const size_t                bytes        = (size_t)VR_KB_WIDTH_PX * VR_KB_HEIGHT_PX * 4;
	const uint64_t              now_ms       = SDL_GetTicks();
	uint32_t                    index        = 0;
	SDL_GPUCommandBuffer*       command_buffer;
	SDL_GPUCopyPass*            copy_pass;
	SDL_GPUTextureTransferInfo  source;
	SDL_GPUTextureRegion        region;
	void*                       mapped;

	if (!g_vr.kb_visible || !g_vr.kb_dirty || !g_vr.frame_begun) {
		return;
	}
	VrKeyboard_Render(&g_vr.kb, g_vr.kb_pixels, g_vr.kb_bgra, now_ms);
	g_vr.kb_drawn_flash = g_vr.kb.flash_key != VR_KB_NO_KEY && now_ms < g_vr.kb.flash_until_ms;

	mapped = SDL_MapGPUTransferBuffer(g_vr.device, g_vr.kb_transfer, true);
	if (!mapped) {
		return;
	}
	memcpy(mapped, g_vr.kb_pixels, bytes);
	SDL_UnmapGPUTransferBuffer(g_vr.device, g_vr.kb_transfer);

	if (!vr_ok(p_xrAcquireSwapchainImage(g_vr.kb_swapchain, &acquire_info, &index), "xrAcquireSwapchainImage(kb)")) {
		return;
	}
	wait_info.timeout = XR_INFINITE_DURATION;
	if (!vr_ok(p_xrWaitSwapchainImage(g_vr.kb_swapchain, &wait_info), "xrWaitSwapchainImage(kb)")) {
		p_xrReleaseSwapchainImage(g_vr.kb_swapchain, &release_info);
		return;
	}
	command_buffer = SDL_AcquireGPUCommandBuffer(g_vr.device);
	if (command_buffer) {
		SDL_zero(source);
		SDL_zero(region);
		source.transfer_buffer = g_vr.kb_transfer;
		source.pixels_per_row  = VR_KB_WIDTH_PX;
		source.rows_per_layer  = VR_KB_HEIGHT_PX;
		region.texture         = g_vr.kb_images[index];
		region.w               = VR_KB_WIDTH_PX;
		region.h               = VR_KB_HEIGHT_PX;
		region.d               = 1;
		copy_pass              = SDL_BeginGPUCopyPass(command_buffer);
		SDL_UploadToGPUTexture(copy_pass, &source, &region, false);
		SDL_EndGPUCopyPass(copy_pass);
		if (SDL_SubmitGPUCommandBuffer(command_buffer)) {
			g_vr.kb_dirty = 0;
		} else {
			Aeron_LogError(VR_LOG, "keyboard upload failed: %s", SDL_GetError());
		}
	}
	p_xrReleaseSwapchainImage(g_vr.kb_swapchain, &release_info);
	g_vr.kb_ever_released = 1;
}

static void vr_create_keyboard(SDL_GPUTextureFormat format, const XrSwapchainCreateInfo* base) {
	XrSwapchainCreateInfo         info     = *base;
	SDL_GPUTransferBufferCreateInfo transfer = { 0 };

	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
					  XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	info.width  = VR_KB_WIDTH_PX;
	info.height = VR_KB_HEIGHT_PX;
	if (!vr_ok(SDL_CreateGPUXRSwapchain(g_vr.device, g_vr.session, &info, format, &g_vr.kb_swapchain, &g_vr.kb_images),
			   "SDL_CreateGPUXRSwapchain(keyboard)")) {
		g_vr.kb_swapchain = XR_NULL_HANDLE;
		g_vr.kb_images    = NULL;
		return;
	}
	transfer.usage    = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer.size     = (Uint32)VR_KB_WIDTH_PX * VR_KB_HEIGHT_PX * 4;
	g_vr.kb_transfer  = SDL_CreateGPUTransferBuffer(g_vr.device, &transfer);
	g_vr.kb_pixels    = SDL_malloc(transfer.size);
	g_vr.kb_bgra      = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
	if (!g_vr.kb_transfer || !g_vr.kb_pixels) {
		Aeron_LogError(VR_LOG, "virtual keyboard unavailable: %s", SDL_GetError());
		SDL_DestroyGPUXRSwapchain(g_vr.device, g_vr.kb_swapchain, g_vr.kb_images);
		g_vr.kb_swapchain = XR_NULL_HANDLE;
		g_vr.kb_images    = NULL;
		return;
	}
	VrKeyboard_Reset(&g_vr.kb);
	Aeron_LogInfo(VR_LOG, "virtual keyboard ready (%dx%d)", VR_KB_WIDTH_PX, VR_KB_HEIGHT_PX);
}

static void vr_destroy_keyboard(SDL_GPUDevice* device) {
	if (g_vr.kb_swapchain) {
		SDL_DestroyGPUXRSwapchain(device, g_vr.kb_swapchain, g_vr.kb_images);
		g_vr.kb_swapchain = XR_NULL_HANDLE;
		g_vr.kb_images    = NULL;
	}
	if (g_vr.kb_transfer) {
		SDL_ReleaseGPUTransferBuffer(device, g_vr.kb_transfer);
		g_vr.kb_transfer = NULL;
	}
	SDL_free(g_vr.kb_pixels);
	g_vr.kb_pixels = NULL;
}

static void vr_update_haptics(void) {
	const uint16_t strength = g_vr.rumble_low > g_vr.rumble_high ? g_vr.rumble_low : g_vr.rumble_high;
	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };

	info.action        = g_vr.act_haptic;
	info.subactionPath = g_vr.hand_path[VR_RIGHT];
	if (strength > 0) {
		XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
		vibration.amplitude         = (float)strength / 65535.0f;
		vibration.duration          = 50000000; /* 50 ms, refreshed every frame */
		vibration.frequency         = XR_FREQUENCY_UNSPECIFIED;
		p_xrApplyHapticFeedback(g_vr.session, &info, (const XrHapticBaseHeader*)&vibration);
	}
}

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

static void vr_update_input(void) {
	const uint64_t       now_ns    = SDL_GetTicksNS();
	const float          dt        = g_vr.last_input_ns ? (float)(now_ns - g_vr.last_input_ns) / 1e9f : 0.0f;
	XrActiveActionSet    active    = { g_vr.action_set, XR_NULL_PATH };
	XrActionsSyncInfo    sync      = { XR_TYPE_ACTIONS_SYNC_INFO };
	int                  in_flight;
	int                  flying;

	g_vr.last_input_ns = now_ns;
	if (!g_vr.focused) {
		return;
	}
	sync.countActiveActionSets = 1;
	sync.activeActionSets      = &active;
	if (XR_FAILED(p_xrSyncActions(g_vr.session, &sync))) {
		return;
	}
	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		g_vr.trigger[hand]     = vr_get_float(g_vr.act_trigger, (VrHand)hand);
		g_vr.grip[hand]        = vr_get_float(g_vr.act_grip, (VrHand)hand);
		g_vr.stick[hand]       = vr_get_stick((VrHand)hand);
		g_vr.stick_click[hand] = vr_get_bool(g_vr.act_stick_click, g_vr.hand_path[hand]);
	}
	g_vr.button_a    = vr_get_bool(g_vr.act_a, XR_NULL_PATH);
	g_vr.button_b    = vr_get_bool(g_vr.act_b, XR_NULL_PATH);
	g_vr.button_x    = vr_get_bool(g_vr.act_x, XR_NULL_PATH);
	g_vr.button_y    = vr_get_bool(g_vr.act_y, XR_NULL_PATH);
	g_vr.button_menu = vr_get_bool(g_vr.act_menu, XR_NULL_PATH);
	vr_autotest_apply_inputs();

	in_flight           = vr_in_flight();
	g_vr.screen_width_m = in_flight ? VR_SCREEN_WIDTH_FLIGHT : VR_SCREEN_WIDTH_MENU;
	/* "flying" = the gamepad drives the craft; otherwise menu controls. */
	flying = in_flight && !vr_flight_menu_active();
	{
		static int prev_flying = -1, block_a, block_b, block_x, block_y, block_menu;
		if (prev_flying >= 0 && prev_flying != flying) {
			block_a    = g_vr.button_a;
			block_b    = g_vr.button_b;
			block_x    = g_vr.button_x;
			block_y    = g_vr.button_y;
			block_menu = g_vr.button_menu;
		}
		prev_flying = flying;
		block_a    = block_a && g_vr.button_a;
		block_b    = block_b && g_vr.button_b;
		block_x    = block_x && g_vr.button_x;
		block_y    = block_y && g_vr.button_y;
		block_menu = block_menu && g_vr.button_menu;
		g_vr.button_a    = g_vr.button_a && !block_a;
		g_vr.button_b    = g_vr.button_b && !block_b;
		g_vr.button_x    = g_vr.button_x && !block_x;
		g_vr.button_y    = g_vr.button_y && !block_y;
		g_vr.button_menu = g_vr.button_menu && !block_menu;
	}

	vr_create_pad();
	vr_update_keyboard(flying);
	vr_update_radials(flying);
	vr_update_pointer();
	vr_autotest_apply_pointer();
	vr_update_mouse_buttons(flying);
	vr_update_gamepad(flying, dt);
	vr_update_menu_keys(flying);
	vr_update_haptics();

	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		g_vr.trigger_down[hand] = vr_hysteresis(g_vr.trigger_down[hand], g_vr.trigger[hand]);
		g_vr.grip_down[hand]    = vr_hysteresis(g_vr.grip_down[hand], g_vr.grip[hand]);
	}
}

/* ------------------------------------------------------------------------ */
/* Frame hooks                                                              */

static void vr_end_xr_frame(void);

static void vr_release_eye_images(void) {
	for (int eye = 0; eye < 2; ++eye) {
		if (g_vr.eye_acquired[eye]) {
			XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			p_xrReleaseSwapchainImage(g_vr.eye_swapchain[eye], &release_info);
			g_vr.eye_acquired[eye] = 0;
		}
	}
}

/* Locates both eyes and hands the renderer two eye images for this frame.
 * The renderer only uses them while the 3D flight scene is on screen. */
static void vr_prepare_stereo(void) {
	XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState      state  = { XR_TYPE_VIEW_STATE };
	XvtVrStereoFrame frame;
	uint32_t         count  = 0;

	g_vr.views_valid      = 0;
	g_vr.stereo_published = 0;
	XvtRemasterVr_SetStereoFrame(NULL);
	if (!g_vr.frame_state.shouldRender || g_vr.eye_width <= 0 || !vr_in_flight()) {
		return;
	}
	g_vr.views[0].type = g_vr.views[1].type = XR_TYPE_VIEW;
	g_vr.views[0].next = g_vr.views[1].next = NULL;
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime           = g_vr.frame_state.predictedDisplayTime;
	locate.space                 = g_vr.local_space;
	if (XR_FAILED(p_xrLocateViews(g_vr.session, &locate, &state, 2, &count, g_vr.views)) || count != 2 ||
		!(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) ||
		!(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
		return;
	}

	SDL_zero(frame);
	for (int eye = 0; eye < 2; ++eye) {
		XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrSwapchainImageWaitInfo    wait_info    = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		uint32_t                    index        = 0;
		const XrView*               view         = &g_vr.views[eye];
		XvtVrStereoEye*             out          = &frame.eyes[eye];

		if (!vr_ok(p_xrAcquireSwapchainImage(g_vr.eye_swapchain[eye], &acquire_info, &index),
				   "xrAcquireSwapchainImage(eye)")) {
			vr_release_eye_images();
			return;
		}
		g_vr.eye_acquired[eye] = 1;
		wait_info.timeout      = XR_INFINITE_DURATION;
		if (!vr_ok(p_xrWaitSwapchainImage(g_vr.eye_swapchain[eye], &wait_info), "xrWaitSwapchainImage(eye)")) {
			vr_release_eye_images();
			return;
		}
		out->position_m[0]  = view->pose.position.x;
		out->position_m[1]  = view->pose.position.y;
		out->position_m[2]  = view->pose.position.z;
		out->orientation[0] = view->pose.orientation.x;
		out->orientation[1] = view->pose.orientation.y;
		out->orientation[2] = view->pose.orientation.z;
		out->orientation[3] = view->pose.orientation.w;
		out->tan_left       = tanf(-view->fov.angleLeft);
		out->tan_right      = tanf(view->fov.angleRight);
		out->tan_up         = tanf(view->fov.angleUp);
		out->tan_down       = tanf(-view->fov.angleDown);
		out->target         = g_vr.eye_images[eye][index];
		out->target_width   = g_vr.eye_width;
		out->target_height  = g_vr.eye_height;
	}
	g_vr.head_pos.x       = (g_vr.views[0].pose.position.x + g_vr.views[1].pose.position.x) * 0.5f;
	g_vr.head_pos.y       = (g_vr.views[0].pose.position.y + g_vr.views[1].pose.position.y) * 0.5f;
	g_vr.head_pos.z       = (g_vr.views[0].pose.position.z + g_vr.views[1].pose.position.z) * 0.5f;
	g_vr.views_valid      = 1;
	g_vr.stereo_published = 1;
	XvtRemasterVr_SetStereoFrame(&frame);
}

static int vr_begin_frame(void* userdata) {
	XrFrameWaitInfo  wait_info  = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };

	(void)userdata;
	if (g_vr.frame_begun) {
		/* Previous frame never reached Present: close it before starting anew. */
		vr_end_xr_frame();
	}
	vr_poll_events();
	if (!g_vr.running) {
		/* Not visible yet (or paused): idle without spinning the CPU. */
		SDL_Delay(10);
		return 1;
	}
	g_vr.frame_state.type = XR_TYPE_FRAME_STATE;
	g_vr.frame_state.next = NULL;
	if (!vr_ok(p_xrWaitFrame(g_vr.session, &wait_info, &g_vr.frame_state), "xrWaitFrame")) {
		return 1;
	}
	if (!vr_ok(p_xrBeginFrame(g_vr.session, &begin_info), "xrBeginFrame")) {
		return 1;
	}
	g_vr.frame_begun = 1;
	vr_update_input();
	vr_upload_keyboard();
	vr_upload_radials();
	vr_prepare_stereo();
	return 1;
}

static int vr_acquire(SDL_GPUCommandBuffer* command_buffer, SDL_GPUTexture** texture, void* userdata) {
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo    wait_info    = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	uint32_t                    index        = 0;

	(void)command_buffer;
	(void)userdata;
	*texture = NULL;
	/* Stereo flight: the screen frame carries only the HUD over the eyes. */
	Aeron_SetExternalTransparentClear(XvtRemasterVr_StereoRendered());
	if (!g_vr.frame_begun || !g_vr.frame_state.shouldRender || g_vr.image_acquired) {
		return 1;
	}
	if (!vr_ok(p_xrAcquireSwapchainImage(g_vr.swapchain, &acquire_info, &index), "xrAcquireSwapchainImage")) {
		return 1;
	}
	wait_info.timeout = XR_INFINITE_DURATION;
	if (!vr_ok(p_xrWaitSwapchainImage(g_vr.swapchain, &wait_info), "xrWaitSwapchainImage")) {
		XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		p_xrReleaseSwapchainImage(g_vr.swapchain, &release_info);
		return 1;
	}
	g_vr.image_acquired = 1;
	*texture            = g_vr.images[index];
	return 1;
}

static void vr_end_xr_frame(void) {
	XrFrameEndInfo                     end_info = { XR_TYPE_FRAME_END_INFO };
	XrCompositionLayerQuad             quad        = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad             kb_quad     = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerProjection       projection  = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView   proj_views[2];
	XrCompositionLayerQuad             radial_quad[VR_HAND_COUNT];
	const XrCompositionLayerBaseHeader* layers[3 + VR_HAND_COUNT];
	uint32_t                           layer_count = 0;
	const int stereo = g_vr.stereo_published && g_vr.views_valid && XvtRemasterVr_StereoRendered();
	float     hud_tan[4];

	vr_release_eye_images();
	g_vr.stereo_published = 0;

	if (g_vr.image_acquired) {
		XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		p_xrReleaseSwapchainImage(g_vr.swapchain, &release_info);
		g_vr.image_acquired = 0;
		if (!g_vr.image_ever_released) {
			Aeron_LogInfo(VR_LOG, "first frame presented to the virtual screen");
		}
		g_vr.image_ever_released = 1;
	}
	if (!g_vr.frame_begun) {
		return;
	}
	g_vr.frame_begun = 0;

	if (g_vr.frame_state.shouldRender) {
		if (stereo) {
			for (int eye = 0; eye < 2; ++eye) {
				SDL_zero(proj_views[eye]);
				proj_views[eye].type                            = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				proj_views[eye].pose                            = g_vr.views[eye].pose;
				proj_views[eye].fov                             = g_vr.views[eye].fov;
				proj_views[eye].subImage.swapchain              = g_vr.eye_swapchain[eye];
				proj_views[eye].subImage.imageRect.extent.width  = g_vr.eye_width;
				proj_views[eye].subImage.imageRect.extent.height = g_vr.eye_height;
			}
			projection.space      = g_vr.local_space;
			projection.viewCount  = 2;
			projection.views      = proj_views;
			layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&projection;
		}
		if (g_vr.image_ever_released) {
			quad.layerFlags                       = 0;
			quad.space                            = g_vr.local_space;
			quad.eyeVisibility                    = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain               = g_vr.swapchain;
			quad.subImage.imageRect.offset.x      = 0;
			quad.subImage.imageRect.offset.y      = 0;
			quad.subImage.imageRect.extent.width  = VR_SCREEN_WIDTH_PX;
			quad.subImage.imageRect.extent.height = VR_SCREEN_HEIGHT_PX;
			quad.subImage.imageArrayIndex         = 0;
			quad.pose.orientation.w               = 1.0f;
			quad.pose.position.z                  = -VR_SCREEN_DISTANCE;
			quad.size.width                       = g_vr.screen_width_m;
			quad.size.height                      = vr_screen_height_m();
			if (stereo && XvtRemasterVr_HudTangents(&hud_tan[0], &hud_tan[1], &hud_tan[2], &hud_tan[3])) {
				/* HUD overlay: the game camera's frustum seen from the seat, so
				 * target boxes sit on their ships. Premultiplied alpha. */
				quad.layerFlags      = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				quad.pose.position.x = VR_HUD_DISTANCE * (hud_tan[1] - hud_tan[0]) * 0.5f;
				quad.pose.position.y = VR_HUD_DISTANCE * (hud_tan[2] - hud_tan[3]) * 0.5f;
				quad.pose.position.z = -VR_HUD_DISTANCE;
				quad.size.width      = VR_HUD_DISTANCE * (hud_tan[0] + hud_tan[1]);
				quad.size.height     = VR_HUD_DISTANCE * (hud_tan[2] + hud_tan[3]);
			}
			layers[layer_count++]                 = (const XrCompositionLayerBaseHeader*)&quad;
		}
		if (g_vr.kb_visible && g_vr.kb_ever_released) {
			/* Premultiplied alpha, drawn over the screen. */
			kb_quad.layerFlags                       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			kb_quad.space                            = g_vr.local_space;
			kb_quad.eyeVisibility                    = XR_EYE_VISIBILITY_BOTH;
			kb_quad.subImage.swapchain               = g_vr.kb_swapchain;
			kb_quad.subImage.imageRect.extent.width  = VR_KB_WIDTH_PX;
			kb_quad.subImage.imageRect.extent.height = VR_KB_HEIGHT_PX;
			kb_quad.pose.orientation                 = vr_kb_orientation();
			kb_quad.pose.position.y                  = VR_KB_HEIGHT;
			kb_quad.pose.position.z                  = -VR_KB_DISTANCE;
			kb_quad.size.width                       = VR_KB_WIDTH_M;
			kb_quad.size.height                      = vr_kb_height_m();
			layers[layer_count++]                    = (const XrCompositionLayerBaseHeader*)&kb_quad;
		}
		for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
			const VrRadialState* radial = &g_vr.radial[hand];
			XrCompositionLayerQuad* rq  = &radial_quad[hand];
			if (!radial->visible || !radial->pose_valid || !radial->panel.ever_released) {
				continue;
			}
			SDL_zero(*rq);
			/* Premultiplied alpha, drawn over everything. */
			rq->type                           = XR_TYPE_COMPOSITION_LAYER_QUAD;
			rq->layerFlags                     = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			rq->space                          = g_vr.local_space;
			rq->eyeVisibility                  = XR_EYE_VISIBILITY_BOTH;
			rq->subImage.swapchain             = radial->panel.swapchain;
			rq->subImage.imageRect.extent.width  = radial->panel.width;
			rq->subImage.imageRect.extent.height = radial->panel.height;
			rq->pose                           = radial->pose;
			rq->size.width                     = VR_RADIAL_SIZE_M;
			rq->size.height                    = VR_RADIAL_SIZE_M;
			layers[layer_count++]              = (const XrCompositionLayerBaseHeader*)rq;
		}
	}
	end_info.displayTime          = g_vr.frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount           = layer_count;
	end_info.layers               = layers;
	vr_ok(p_xrEndFrame(g_vr.session, &end_info), "xrEndFrame");
}

static void vr_end_frame(int composed, void* userdata) {
	(void)composed;
	(void)userdata;
	vr_end_xr_frame();
}

static void vr_shutdown(SDL_GPUDevice* device, void* userdata) {
	(void)userdata;
	vr_release_all_keys();
	if (g_vr.frame_begun || g_vr.image_acquired) {
		vr_end_xr_frame();
	}
	vr_release_eye_images();
	vr_destroy_keyboard(device);
	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		vr_panel_destroy(device, &g_vr.radial[hand].panel);
	}
	for (int eye = 0; eye < 2; ++eye) {
		if (g_vr.eye_swapchain[eye]) {
			SDL_DestroyGPUXRSwapchain(device, g_vr.eye_swapchain[eye], g_vr.eye_images[eye]);
			g_vr.eye_swapchain[eye] = XR_NULL_HANDLE;
			g_vr.eye_images[eye]    = NULL;
		}
	}
	if (g_vr.swapchain) {
		SDL_DestroyGPUXRSwapchain(device, g_vr.swapchain, g_vr.images);
		g_vr.swapchain = XR_NULL_HANDLE;
		g_vr.images    = NULL;
	}
	if (g_vr.session) {
		if (g_vr.running) {
			p_xrEndSession(g_vr.session);
		}
		p_xrDestroySession(g_vr.session);
		g_vr.session = XR_NULL_HANDLE;
	}
	if (g_vr.pad) {
		SDL_CloseJoystick(g_vr.pad);
		g_vr.pad = NULL;
	}
	if (g_vr.pad_id) {
		SDL_DetachVirtualJoystick(g_vr.pad_id);
		g_vr.pad_id = 0;
	}
}

int XvtVr_Install(void) {
	AeronExternalPresent hooks;

	if (SDL_GetHintBoolean("OPENXVT_VR_DISABLE", false)) {
		Aeron_LogInfo(VR_LOG, "VR disabled by hint; using the flat window");
		return 0;
	}
	SDL_zero(g_vr);
	SDL_zero(hooks);
	SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, VR_PAD_ID_HINT);
	SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
	hooks.configure_device = vr_configure_device;
	hooks.device_created   = vr_device_created;
	hooks.begin_frame      = vr_begin_frame;
	hooks.acquire          = vr_acquire;
	hooks.end_frame        = vr_end_frame;
	hooks.shutdown         = vr_shutdown;
	Aeron_SetExternalPresent(&hooks);
	Aeron_LogInfo(VR_LOG, "OpenXR presentation installed");
	return 1;
}

#else

int XvtVr_Install(void) { return 0; }

#endif
