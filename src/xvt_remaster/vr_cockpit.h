#ifndef XVT_REMASTER_VR_COCKPIT_H
#define XVT_REMASTER_VR_COCKPIT_H

/* 3D cockpits for VR flight from X-Wing Revival (the cockpit OBJ files in
 * Models/ plus Textures/; upgraded X-Wing Alliance cockpits from the XWA
 * Upgrade project), loaded and converted at runtime.
 * Searched in $OPENXVT_REVIVAL_DIR, /sdcard/OpenXvT/XWingRevival, then
 * the copy bundled in the APK. While one is
 * drawn the original cockpit artwork is hidden (hud_draw.c) and only the
 * instruments remain. */

#include "xvt_remaster/flight.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the player craft's cockpit (no render pass may be open on `cmd`).
 * Returns 1 when a 3D cockpit will be drawn this frame. */
int  XvtVrCockpit_Prepare(AeronCommandBuffer* cmd, const XvtRenderSnapshot* snapshot);
/* Adds the cockpit to a scene being built for the eye at `camera_pos`
 * (scene-local world units). */
void XvtVrCockpit_Submit(AeronScene3D* scene, const XvtRenderSnapshot* snapshot, const XvtPreparedFlight* frame,
						 const float camera_pos[3]);
int  XvtVrCockpit_Active(void);
void XvtVrCockpit_SelfTest(void);
void XvtVrCockpit_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
