#ifndef __UI_FONTS_H__
#define __UI_FONTS_H__

/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif
/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* The project's monospace fonts with a symbol font behind them.
 *
 * Font_Mono_Bold_* cover ASCII 32..126 and nothing else, so any label that both
 * sets one of them and contains an LV_SYMBOL_* glyph draws an empty placeholder
 * box where the icon should be. These are the same fonts with `fallback` set to
 * a Montserrat of matching size, which is where LVGL keeps its icon glyphs.
 *
 * They are copies rather than edits to the generated asset files, so
 * regenerating a font does not quietly lose the fallback.
 *
 * Use these everywhere in the UI in preference to Font_Mono_Bold_* directly. */
extern lv_font_t ui_font_mono_14;
extern lv_font_t ui_font_mono_15;
extern lv_font_t ui_font_mono_16;
extern lv_font_t ui_font_mono_17;
extern lv_font_t ui_font_mono_18;
extern lv_font_t ui_font_mono_19;
extern lv_font_t ui_font_mono_20;

/* For the reaction picker: the same idea, but ending in the larger emoji font
 * so the choices are big enough to aim at. */
extern lv_font_t ui_font_react;

/* Must run before any label is created. */
void ui_fonts_init(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
