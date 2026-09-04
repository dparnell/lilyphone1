/**
 * Symbol support for the project's monospace fonts.
 *
 * Font_Mono_Bold_* were generated with the ASCII range only, so a label that
 * sets one of them and contains an LV_SYMBOL_* glyph gets LVGL's placeholder
 * rectangle instead of the icon. That affected most of the phone UI, where the
 * buttons and list rows are all mono and most of them carry an icon.
 *
 * LVGL resolves a missing glyph through `lv_font_t::fallback` and the software
 * renderer draws the bitmap from `resolved_font`, so pointing each mono font at
 * a Montserrat of matching size is enough - Montserrat is where LVGL keeps its
 * icon glyphs. The generated structs are const and their fallback is NULL, so
 * these are runtime copies with the field filled in; editing the asset files
 * instead would be undone the next time a font is regenerated.
 */
#include "ui_fonts.h"
#include "assets.h"

/* Montserrat with the emoji font behind it. LVGL resolves a missing glyph down
 * a chain, so the mono fonts fall back to these and these fall back to emoji:
 * text -> LVGL's icons -> emoji. Montserrat is const, hence the copies. */
static lv_font_t ui_font_symbols_14;
static lv_font_t ui_font_symbols_18;

lv_font_t ui_font_mono_14;
lv_font_t ui_font_mono_15;
lv_font_t ui_font_mono_16;
lv_font_t ui_font_mono_17;
lv_font_t ui_font_mono_18;
lv_font_t ui_font_mono_19;
lv_font_t ui_font_mono_20;

static void font_with_symbols(lv_font_t *dst, const lv_font_t *src, const lv_font_t *symbols)
{
    *dst = *src;
    dst->fallback = symbols;
}

void ui_fonts_init(void)
{
    // One emoji size for the whole chain. They are drawn to sit on a 16px line,
    // which is close enough to every text size the UI uses.
    font_with_symbols(&ui_font_symbols_14, &lv_font_montserrat_14, &Font_Emoji_16);
    font_with_symbols(&ui_font_symbols_18, &lv_font_montserrat_18, &Font_Emoji_16);

    // Only Montserrat 14, 18 and 26 are compiled in (see lv_conf.h), so the
    // smaller half of the range falls back to 14 and the larger to 18. The
    // icons end up within a pixel or two of the text they sit beside.
    font_with_symbols(&ui_font_mono_14, &Font_Mono_Bold_14, &ui_font_symbols_14);
    font_with_symbols(&ui_font_mono_15, &Font_Mono_Bold_15, &ui_font_symbols_14);
    font_with_symbols(&ui_font_mono_16, &Font_Mono_Bold_16, &ui_font_symbols_14);
    font_with_symbols(&ui_font_mono_17, &Font_Mono_Bold_17, &ui_font_symbols_18);
    font_with_symbols(&ui_font_mono_18, &Font_Mono_Bold_18, &ui_font_symbols_18);
    font_with_symbols(&ui_font_mono_19, &Font_Mono_Bold_19, &ui_font_symbols_18);
    font_with_symbols(&ui_font_mono_20, &Font_Mono_Bold_20, &ui_font_symbols_18);
}
