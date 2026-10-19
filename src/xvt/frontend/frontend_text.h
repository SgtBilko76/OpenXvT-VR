#ifndef XVT_FRONTEND_FRONTEND_TEXT_H
#define XVT_FRONTEND_FRONTEND_TEXT_H

#include "xvt/util/win32.h"
#include "xvt/xvt_typedefs.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct BitmapFont {
	uint8_t* pGlyphBits; ///< Runtime pointer to the compressed glyph-byte blob; serialized in the .ABP header
						 ///< as the blob byte count.
	unsigned int glyphBitOffset[256]; ///< Per-character byte offsets into pGlyphBits.
	uint8_t glyphHeight[256];
	uint8_t glyphWidth[256];
	unsigned int pointSize;
	uint8_t inUse; ///< Font-slot occupancy flag; scanned when allocating and cleared when freeing.
	uint8_t charSpacing;
	uint8_t field_60A; ///< Set to 1 for generated fonts and persisted in the 1547-byte .ABP header; no
					   ///< runtime consumer is identified.
};

typedef uint16_t GlyphScratchBuffer[65536];

extern int g_activeTextFieldId;
extern unsigned int g_textFieldEditSerial;

int FrontendText_DrawEditableField(RECT* rect, char* text, int maxChars, int fieldId, unsigned int fontSize,
								   const char* ignoredChars);
int FrontendText_LoadFont(int pointSize);
void FrontendText_FreeAllFonts(void);
void FrontendText_FreeFont(unsigned int pointSize);
int FrontendText_Draw(int fontSize, const char* str, int x, int y, int color);
int FrontendText_DrawCentered(int fontSize, const char* str, RECT* rect, int color);
int FrontendText_DrawAlignedInRect(int fontSize, const char* str, RECT* rect, int centerH, int centerV,
								   int color);
int FrontendText_DrawLineArrayInRect(int fontSize, const char** lines, int lineCount, const RECT* rect,
									 int color, int centerHorizontally, int centerVertically,
									 int lineSpacing);
int FrontendText_DrawWrapped(int fontSize, const char* str, RECT* rect, int color, int lineSpacing,
							 int firstVisibleLine);
int FrontendText_GetFontHeight(int fontSize);
int FrontendText_MeasureWidth(const char* str, int fontSize);
void FrontendText_SaveFontAtlasFile(char* fileName, void** font, unsigned int glyphBlobSize);
int FrontendText_LoadFontAtlasFile(const char* fileName, int slotIndex);
int FrontendText_ResetGlyphScratchBuffer(int frames);
int FrontendText_ResetGlyphScratch(void);
int FrontendText_PushGlyphScratchTtl(void);
int FrontendText_PopGlyphScratchTtl(void);
void FrontendText_DrawFormattedWrappedText(RECT* rect, const uint8_t* text, int suppressCenteredHeadings);

#ifdef __cplusplus
}
#endif

#endif
