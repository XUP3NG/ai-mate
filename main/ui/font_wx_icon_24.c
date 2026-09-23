/*******************************************************************************
 * Size: 24 px
 * Bpp: 1
 * Opts: --font C:/Windows/Fonts/seguisym.ttf --symbols ☀☁☂☔☰❄⚡ --size 24 --bpp 1 --format lvgl --no-compress -o font_wx_icon_24.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef FONT_WX_ICON_24
#define FONT_WX_ICON_24 1
#endif

#if FONT_WX_ICON_24

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+2600 "☀" */
    0x0, 0xc0, 0x0, 0x30, 0x3, 0xc, 0x30, 0x60,
    0x1c, 0x19, 0xe6, 0x1, 0xfe, 0x0, 0x7f, 0x80,
    0x3f, 0xf0, 0xef, 0xfd, 0xc3, 0xff, 0x0, 0x7f,
    0x80, 0x1f, 0xe0, 0x19, 0xe6, 0x6, 0x1, 0xc3,
    0x0, 0x30, 0x3, 0x0, 0x0, 0xc0, 0x0, 0x30,
    0x0,

    /* U+2601 "☁" */
    0x0, 0x20, 0x0, 0x7, 0xc0, 0x0, 0x7f, 0xe0,
    0x7, 0xff, 0xc0, 0xff, 0xfe, 0x1f, 0xff, 0xf1,
    0xff, 0xff, 0xdf, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xdf, 0xff, 0xfc, 0x7f, 0xff, 0xc0,

    /* U+2602 "☂" */
    0x1, 0xc0, 0x7, 0xfc, 0xf, 0xff, 0x8f, 0xff,
    0xef, 0xff, 0xf0, 0x84, 0x4, 0x2, 0x0, 0x1,
    0x0, 0x0, 0x80, 0x0, 0x40, 0x0, 0x20, 0x0,
    0x10, 0x0, 0x8, 0x0, 0x4, 0x0, 0x2, 0x0,
    0x11, 0x0, 0x8, 0x80, 0x3, 0x80, 0x0,

    /* U+2614 "☔" */
    0x10, 0x20, 0x30, 0x60, 0x64, 0xc0, 0x8, 0x0,
    0x10, 0x18, 0x0, 0xb0, 0x1, 0x83, 0xe0, 0x1f,
    0xf0, 0x7f, 0xf1, 0xff, 0xf3, 0xff, 0xe4, 0xc6,
    0x40, 0x20, 0x40, 0x40, 0x0, 0x80, 0x1, 0x0,
    0x2, 0x0, 0x4, 0x0, 0x8, 0x0, 0x10, 0x0,
    0x20, 0x1, 0x80, 0x0,

    /* U+2630 "☰" */
    0xff, 0xff, 0xff, 0xff, 0xc0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1,
    0xff, 0xff, 0xff, 0xff, 0x80,

    /* U+26A1 "⚡" */
    0x0, 0x40, 0x8, 0x6, 0x1, 0x80, 0x60, 0x18,
    0x6, 0x1, 0x80, 0x7f, 0x80, 0x78, 0xe, 0x3,
    0x0, 0xc0, 0x10, 0x4, 0x1, 0x0, 0xc0, 0x0,

    /* U+2744 "❄" */
    0x0, 0x80, 0x3, 0x58, 0x4, 0x79, 0x2, 0x10,
    0x85, 0x8, 0x51, 0xc4, 0x71, 0xf2, 0x7c, 0x7,
    0xc0, 0x0, 0x80, 0x1, 0xf0, 0x1f, 0x27, 0xc7,
    0x11, 0xc5, 0x8, 0x50, 0x84, 0x20, 0x47, 0x90,
    0xd, 0x60, 0x0, 0x80, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 324, .box_w = 18, .box_h = 18, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 41, .adv_w = 334, .box_w = 21, .box_h = 12, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 73, .adv_w = 295, .box_w = 17, .box_h = 18, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 112, .adv_w = 295, .box_w = 15, .box_h = 23, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 156, .adv_w = 345, .box_w = 17, .box_h = 17, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 193, .adv_w = 175, .box_w = 11, .box_h = 17, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 217, .adv_w = 341, .box_w = 17, .box_h = 17, .ofs_x = 2, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x1, 0x2, 0x14, 0x30, 0xa1, 0x144
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 9728, .range_length = 325, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 7, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t font_wx_icon_24 = {
#else
lv_font_t font_wx_icon_24 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 24,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if FONT_WX_ICON_24*/

