/* SPDX-License-Identifier: GPL-3.0-only */
#include "typography.h"
#include "pixels.h"
#include "ui-size.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fribidi.h>
#include <hb.h>
#include <hb-ot.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const unsigned char *data;
    long size;
    hb_script_t script;
} FontAsset;
#include "text-assets.h"

enum { FONT_COUNT = sizeof(font_assets) / sizeof(*font_assets), TEXT_BYTES = 512,
       MAX_GLYPHS = 768, TEXT_CACHE = 96, MASK_CACHE = 1024 };

typedef struct {
    FT_Face ft;
    hb_font_t *hb;
    unsigned px;
} Face;

typedef struct {
    uint32_t glyph;
    unsigned char face;
    int x, y;
} Letter;

typedef struct {
    char text[TEXT_BYTES + 1];
    uint16_t ends[TEXT_BYTES];
    int advances[TEXT_BYTES];
    Letter letters[MAX_GLYPHS];
    unsigned bytes, chars, count;
    Font style;
    int width;
    bool rtl;
} Layout;

typedef struct {
    uint32_t glyph;
    unsigned face, px;
    int left, top, width, height;
    unsigned char *alpha;
} Mask;

typedef struct {
    unsigned first, end, font;
    hb_script_t script;
    FriBidiLevel level;
} Run;

static FT_Library library;
static hb_buffer_t *buffer;
static Face faces[FONT_COUNT];
static Layout layouts[TEXT_CACHE];
static Mask masks[MASK_CACHE];
static const unsigned sizes[FONT_STYLES] = {17, 20, 20, 23, 42, 51, 29, 35, 17};

static Face *font(unsigned index) {
    Face *face = &faces[index];
    if (face->ft) return face;
    if (!library && FT_Init_FreeType(&library)) return NULL;
    const FontAsset *asset = &font_assets[index];
    if (FT_New_Memory_Face(library, asset->data, asset->size, 0, &face->ft)) return NULL;
    hb_blob_t *blob = hb_blob_create((const char *)asset->data, (unsigned)asset->size, HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_face_t *hb_face = hb_face_create(blob, 0);
    face->hb = hb_font_create(hb_face);
    hb_ot_font_set_funcs(face->hb);
    hb_face_destroy(hb_face);
    hb_blob_destroy(blob);
    return face;
}

static uint32_t decode(const unsigned char **at, const unsigned char *end) {
    const unsigned char *p = *at;
    uint32_t first = *p++;
    *at = p;
    if (first < 0x80) return first;
    unsigned extra = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : first >= 0xc2 ? 1 : 0;
    if (!extra || first > 0xf4 || (size_t)(end - p) < extra) return 0xfffd;
    uint32_t value = first & (0x7fu >> (extra + 1));
    for (unsigned i = 0; i < extra; ++i) {
        if ((p[i] & 0xc0) != 0x80) return 0xfffd;
        value = (value << 6) | (p[i] & 0x3f);
    }
    static const uint32_t minimum[] = {0, 0x80, 0x800, 0x10000};
    if (value < minimum[extra] || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0xfffd;
    *at = p + extra;
    return value;
}

static bool specific(hb_script_t script) {
    return script != HB_SCRIPT_COMMON && script != HB_SCRIPT_INHERITED && script != HB_SCRIPT_UNKNOWN;
}

static hb_script_t font_script(hb_script_t script) {
    if (script == HB_SCRIPT_HIRAGANA || script == HB_SCRIPT_KATAKANA || script == HB_SCRIPT_HANGUL ||
        script == HB_SCRIPT_BOPOMOFO) return HB_SCRIPT_HAN;
    return script;
}

static bool has_char(unsigned face, uint32_t value) {
    Face *entry = font(face);
    return entry && FT_Get_Char_Index(entry->ft, value) != 0;
}

static unsigned select_face(unsigned primary, unsigned previous, hb_script_t script, uint32_t *value) {
    const uint32_t cp = *value;
    hb_unicode_funcs_t *unicode = hb_unicode_funcs_get_default();
    if (hb_unicode_general_category(unicode, cp) == HB_UNICODE_GENERAL_CATEGORY_FORMAT ||
        (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef)) return previous;
    if (hb_unicode_combining_class(unicode, cp) && has_char(previous, cp)) return previous;
    if (has_char(primary, cp)) return primary;
    const hb_script_t wanted = font_script(script);
    for (unsigned i = 2; i < FONT_COUNT; ++i)
        if (font_assets[i].script == wanted && has_char(i, cp)) return i;
    for (unsigned i = 2; i < FONT_COUNT; ++i)
        if (has_char(i, cp)) return i;
    *value = 0x25a1;
    if (has_char(primary, *value)) return primary;
    for (unsigned i = 2; i < FONT_COUNT; ++i)
        if (has_char(i, *value)) return i;
    return primary;
}

static void reorder(Run *runs, unsigned count) {
    int high = 0, low = 127;
    for (unsigned i = 0; i < count; ++i) {
        int level = runs[i].level;
        if (level > high) high = level;
        if ((level & 1) && level < low) low = level;
    }
    for (int level = high; level >= low; --level) {
        for (unsigned i = 0; i < count;) {
            if (runs[i].level < level) { ++i; continue; }
            unsigned end = i + 1;
            while (end < count && runs[end].level >= level) ++end;
            for (unsigned a = i, b = end - 1; a < b; ++a, --b) {
                Run swap = runs[a];
                runs[a] = runs[b];
                runs[b] = swap;
            }
            i = end;
        }
    }
}

static hb_language_t language(hb_script_t script) {
    if (script == HB_SCRIPT_HIRAGANA || script == HB_SCRIPT_KATAKANA)
        return hb_language_from_string("ja", -1);
    if (script == HB_SCRIPT_HANGUL) return hb_language_from_string("ko", -1);
    return hb_language_from_string("und", -1);
}

static void shape_line(Layout *line) {
    /* Font and layout caches already belong to the UI thread. Keep the
       shaping workspace with them so a font operation cannot exhaust the
       launcher's callback stack. */
    static uint32_t codepoints[TEXT_BYTES];
    static hb_script_t scripts[TEXT_BYTES];
    static FriBidiCharType types[TEXT_BYTES];
    static FriBidiBracketType brackets[TEXT_BYTES];
    static FriBidiLevel levels[TEXT_BYTES];
    static Run runs[TEXT_BYTES];
    const unsigned char *p = (const unsigned char *)line->text, *end = p + line->bytes;
    const unsigned char *start = p;
    hb_unicode_funcs_t *unicode = hb_unicode_funcs_get_default();
    line->chars = line->count = 0;
    line->width = 0;
    line->rtl = false;
    while (p < end) {
        unsigned i = line->chars++;
        uint32_t cp = decode(&p, end);
        codepoints[i] = cp;
        scripts[i] = hb_unicode_script(unicode, cp);
        line->ends[i] = (uint16_t)(p - start);
        line->advances[i] = 0;
    }
    unsigned n = line->chars;
    if (!n) return;
    if (!buffer) buffer = hb_buffer_create();

    FriBidiParType base = FRIBIDI_PAR_ON;
    fribidi_get_bidi_types(codepoints, n, types);
    fribidi_get_bracket_types(codepoints, n, types, brackets);
    if (!fribidi_get_par_embedding_levels_ex(types, brackets, n, &base, levels))
        memset(levels, 0, n * sizeof(*levels));
    line->rtl = FRIBIDI_IS_RTL(base);
    hb_script_t context = HB_SCRIPT_LATIN;
    for (unsigned i = 0; i < n; ++i) {
        if (specific(scripts[i])) { context = scripts[i]; break; }
    }

    unsigned run_count = 0;
    unsigned primary = line->style >= LABEL ? 1 : 0;
    unsigned previous = primary;
    for (unsigned i = 0; i < n; ++i) {
        if (specific(scripts[i])) context = scripts[i];
        unsigned selected = select_face(primary, previous, context, &codepoints[i]);
        Run *last = run_count ? &runs[run_count - 1] : NULL;
        if (last && last->font == selected && last->script == context && last->level == levels[i])
            last->end = i + 1;
        else runs[run_count++] = (Run){i, i + 1, selected, context, levels[i]};
        previous = selected;
    }
    reorder(runs, run_count);

    const unsigned px = ui_px(sizes[line->style]);
    int pen = 0;
    for (unsigned i = 0; i < run_count; ++i) {
        const Run *run = &runs[i];
        Face *chosen = font(run->font);
        if (!chosen) continue;
        hb_font_set_scale(chosen->hb, (int)px * 64, (int)px * 64);
        hb_buffer_reset(buffer);
        hb_buffer_set_direction(buffer, (run->level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buffer, run->script);
        hb_buffer_set_language(buffer, language(run->script));
        hb_buffer_set_flags(buffer, HB_BUFFER_FLAG_REMOVE_DEFAULT_IGNORABLES);
        hb_buffer_add_codepoints(buffer, codepoints, (int)n, run->first, (int)(run->end - run->first));
        hb_shape(chosen->hb, buffer, NULL, 0);
        unsigned length;
        const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &length);
        const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buffer, NULL);
        for (unsigned j = 0; j < length; ++j) {
            if (line->count == MAX_GLYPHS) break;
            Letter *letter = &line->letters[line->count++];
            *letter = (Letter){info[j].codepoint, run->font, pen + pos[j].x_offset, pos[j].y_offset};
            if (info[j].cluster < n) line->advances[info[j].cluster] += pos[j].x_advance;
            pen += pos[j].x_advance;
        }
    }
    line->width = (pen + 63) / 64;
}

static Layout *layout(Font style, const char *text) {
    uint32_t hash = 2166136261u ^ (unsigned)style;
    unsigned bytes = 0;
    while (bytes < TEXT_BYTES && text[bytes]) hash = (hash ^ (unsigned char)text[bytes++]) * 16777619u;
    Layout *entry = &layouts[hash % TEXT_CACHE];
    if (entry->style == style && entry->bytes == bytes && !memcmp(entry->text, text, bytes)) return entry;
    memcpy(entry->text, text, bytes);
    entry->text[bytes] = 0;
    entry->bytes = bytes;
    entry->style = style;
    shape_line(entry);
    return entry;
}

int typography_width(Font style, const char *text) {
    if (!text) return 0;
    return (layout(style, text)->width * UI_SCALE_DEN + UI_SCALE_NUM - 1) / UI_SCALE_NUM;
}

static unsigned fit_text(const Layout *line, int width, bool wrap) {
    if (line->width <= width) return line->bytes;
    unsigned last = 0, word = 0;
    int pen = 0;
    for (unsigned i = 0; i < line->chars; ++i) {
        if (pen + line->advances[i] > width * 64) break;
        unsigned begin = i ? line->ends[i - 1] : 0;
        if (wrap && line->text[begin] == ' ' && begin) word = begin;
        last = line->ends[i];
        pen += line->advances[i];
    }
    return word && word >= last / 2 ? word : last;
}

size_t typography_break(Font style, const char *text, int width) {
    Layout *line = layout(style, text);
    unsigned last = fit_text(line, ui_px(width), true);
    /* Always advance on tightly constrained layouts. */
    return last || !line->chars ? last : line->ends[0];
}

static Mask *bitmap(unsigned index, unsigned px, uint32_t glyph) {
    unsigned key = (glyph * 2654435761u ^ index * 257u ^ px * 8191u) % MASK_CACHE;
    Mask *mask = &masks[key];
    if (mask->px == px && mask->face == index && mask->glyph == glyph) return mask;
    free(mask->alpha);
    *mask = (Mask){.glyph = glyph, .face = index, .px = px};
    Face *chosen = font(index);
    if (!chosen) return mask;
    if (chosen->px != px) {
        if (FT_Set_Pixel_Sizes(chosen->ft, 0, px)) return mask;
        chosen->px = px;
    }
    if (FT_Load_Glyph(chosen->ft, glyph, FT_LOAD_DEFAULT) ||
        FT_Render_Glyph(chosen->ft->glyph, FT_RENDER_MODE_NORMAL)) return mask;
    const FT_GlyphSlot slot = chosen->ft->glyph;
    const FT_Bitmap *image = &slot->bitmap;
    if (!image->width || !image->rows || image->width > 128 || image->rows > 128 ||
        image->pixel_mode != FT_PIXEL_MODE_GRAY) return mask;
    mask->alpha = malloc((size_t)image->width * image->rows);
    if (!mask->alpha) return mask;
    mask->width = (int)image->width;
    mask->height = (int)image->rows;
    mask->left = slot->bitmap_left;
    mask->top = slot->bitmap_top;
    for (int row = 0; row < mask->height; ++row) {
        const unsigned char *src = image->buffer + row * image->pitch;
        memcpy(mask->alpha + row * mask->width, src, (size_t)mask->width);
    }
    return mask;
}

typedef struct {
    uint32_t *pixels;
    unsigned stride;
    int left, right, height, baseline;
    uint32_t color;
} Ink;

static void paint(const Layout *line, Ink ink, int origin) {
    const unsigned px = ui_px(sizes[line->style]);
    for (unsigned i = 0; i < line->count; ++i) {
        const Letter *letter = &line->letters[i];
        int x_start = origin + (letter->x + 32) / 64;
        if (x_start > ink.right + (int)px || x_start < ink.left - (int)px) continue;
        const Mask *mask = bitmap(letter->face, px, letter->glyph);
        int x = x_start + mask->left, y = ink.baseline - (letter->y + 32) / 64 - mask->top;
        for (int row = 0; row < mask->height; ++row) {
            if (y + row < 0 || y + row >= ink.height) continue;
            uint32_t *dst = ink.pixels + (y + row) * ink.stride;
            const unsigned char *alpha = mask->alpha + row * mask->width;
            for (int col = 0; col < mask->width; ++col) {
                if (x + col < ink.left || x + col >= ink.right) continue;
                unsigned a = alpha[col];
                if (!a) continue;
                dst[x + col] = over_opaque(dst[x + col], ink.color, a);
            }
        }
    }
}

void typography_draw(uint32_t *target, unsigned stride, int canvas_width, int canvas_height,
                     int x, int baseline, int width, Font style, const char *text, uint32_t color) {
    if (!text || !*text || width <= 0 || x >= canvas_width || x + width <= 0) return;
    Layout *line = layout(style, text);
    int left = x < 0 ? 0 : x;
    int right = x + width > canvas_width ? canvas_width : x + width;
    Ink ink = {target, stride, left, right, canvas_height, baseline, color};
    if (line->width <= width) { paint(line, ink, x); return; }

    /* Obtain the ellipsis width before reloading a possibly colliding cache entry. */
    int dots = layout(style, "…")->width;
    if (dots >= width) return;
    line = layout(style, text);
    bool rtl = line->rtl;
    unsigned bytes = fit_text(line, width - dots, false);
    char prefix[TEXT_BYTES + 1];
    memcpy(prefix, line->text, bytes);
    prefix[bytes] = 0;
    if (rtl) {
        int old_left = ink.left;
        if (ink.left < x + dots) ink.left = x + dots;
        line = layout(style, prefix);
        paint(line, ink, x + width - line->width);
        ink.left = old_left;
        paint(layout(style, "…"), ink, x);
    } else {
        int old_right = ink.right;
        if (ink.right > x + width - dots) ink.right = x + width - dots;
        line = layout(style, prefix);
        paint(line, ink, x);
        int edge = line->width < width - dots ? x + line->width : x + width - dots;
        ink.right = old_right;
        paint(layout(style, "…"), ink, edge);
    }
}

void typography_close(void) {
    for (unsigned i = 0; i < MASK_CACHE; ++i) free(masks[i].alpha);
    for (unsigned i = 0; i < FONT_COUNT; ++i) {
        if (faces[i].hb) hb_font_destroy(faces[i].hb);
        if (faces[i].ft) FT_Done_Face(faces[i].ft);
    }
    if (buffer) hb_buffer_destroy(buffer);
    if (library) FT_Done_FreeType(library);
}
