#ifndef XVT_REMASTER_VIEW_MODE_H
#define XVT_REMASTER_VIEW_MODE_H
#include "aeron/aeron.h"
#include "xvt_runtime/snapshot/render_snapshot.h"
#ifdef __cplusplus
extern "C" {
#endif
void XvtRemasterView_Init(void);
void XvtRemasterView_BeginFrame(const AeronInputSnapshot* input);
int XvtRemasterView_NeedsWorld(const XvtRenderSnapshot* snapshot);
int XvtRemasterView_Direct(const XvtRenderSnapshot* snapshot, int width, int height);
/* The modern renderer is selected (VR stereo needs it). */
int XvtRemasterView_ModernRequested(void);
void XvtRemasterView_Present(const XvtRenderSnapshot* snapshot, int32_t delta_us, int world_ready,
							 int direct);
void XvtRemasterView_Shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
