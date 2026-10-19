#ifndef XVT_REMASTER_FLIGHT_PIPELINE_H
#define XVT_REMASTER_FLIGHT_PIPELINE_H
#include "xvt_remaster/flight.h"
struct SDL_GPUTexture;
#ifdef __cplusplus
extern "C" {
#endif
void XvtFlightPipeline_Post(AeronScene3D* scene, float shutter, int motion);
int XvtFlightPipeline_Begin(AeronScene3D* scene, const XvtRenderSnapshot* snapshot,
							const XvtPreparedFlight* frame, int reset);
int XvtFlightPipeline_Finish(AeronCommandBuffer* cmd, AeronScene3D* scene);
int XvtFlightPipeline_Resolve(AeronCommandBuffer* cmd, AeronTexture* color, int width, int height, int bloom);
AeronTexture* XvtFlightPipeline_Output(void);
void XvtFlightPipeline_Shutdown(void);
int XvtFlightPipeline_SetDirect(int enabled, int width, int height);
int XvtFlightPipeline_Retain(AeronCommandBuffer* cmd);
int XvtFlightPipeline_SubmitDirect(void);
int XvtFlightPipeline_NeedsRetain(void);
void XvtFlightPipeline_ForgetSources(void);
int XvtFlightPipeline_DrawStandaloneHud(AeronCommandBuffer* cmd, int width, int height);
int XvtFlightPipeline_PrepareSceneResources(AeronScene3D* scene, int flight);
/* VR stereo: eye-mode post settings (no temporal/SSAO/motion blur), rendering
 * a built eye scene into a host eye image, and the HUD-only overlay as the
 * presentation output. */
void XvtFlightPipeline_SetVrEye(int enabled);
int XvtFlightPipeline_ResolveVrEye(AeronCommandBuffer* cmd, AeronScene3D* scene, struct SDL_GPUTexture* destination,
								   int destination_width, int destination_height);
int XvtFlightPipeline_DrawVrHud(AeronCommandBuffer* cmd, int width, int height);
#ifdef __cplusplus
}
#endif
#endif
