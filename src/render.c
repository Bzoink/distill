#include "render.h"

#include "paint.h"

#include "cutil.h"
#include "dfs.h"
#include "shaper.h"

#include <ctype.h>
#include <errno.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
    char *path;
    char *family;
    char *style;
    int weight;
    bool italic;
    bool monospace;
} DFontFile;

struct DRenderContext {
    int width;
    int height;
    DPaint *paint;
    FT_Library freetype;
    DFontFile *files;
    size_t file_count;
    size_t file_capacity;
    unsigned int clip_depth;
};

struct DRenderFont {
    DRenderContext *context;
    FT_Face face;
    hb_font_t *harfbuzz;
    int size;
    int ascent;
    int descent;
    int height;
    int x_height;
    unsigned int decoration;
};

static char *copy_string(const char *text)
{
    size_t length = text ? strlen(text) : 0;
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    if (length) memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static bool has_font_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return d_casecmp(dot, ".ttf") == 0 || d_casecmp(dot, ".otf") == 0 ||
           d_casecmp(dot, ".ttc") == 0;
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    if (!text || !needle || !*needle) return false;
    size_t needle_length = strlen(needle);
    for (const char *cursor = text; *cursor; ++cursor) {
        if (d_ncasecmp(cursor, needle, needle_length) == 0) return true;
    }
    return false;
}

static int style_weight(const char *style, FT_Long face_style_flags)
{
    if ((face_style_flags & FT_STYLE_FLAG_BOLD) ||
        contains_case_insensitive(style, "bold") || contains_case_insensitive(style, "black")) return 700;
    if (contains_case_insensitive(style, "light") || contains_case_insensitive(style, "thin")) return 300;
    return 400;
}

static bool style_italic(const char *style, FT_Long face_style_flags)
{
    return (face_style_flags & FT_STYLE_FLAG_ITALIC) ||
           contains_case_insensitive(style, "italic") || contains_case_insensitive(style, "oblique");
}

static void add_font_file(DRenderContext *context, const char *path)
{
    FT_Face face = NULL;
    if (FT_New_Face(context->freetype, path, 0, &face) != 0) return;
    const char *family = face->family_name ? face->family_name : "";
    const char *style = face->style_name ? face->style_name : "";
    if (context->file_count == context->file_capacity) {
        size_t capacity = context->file_capacity ? context->file_capacity * 2 : 32;
        DFontFile *files = realloc(context->files, capacity * sizeof(*files));
        if (!files) {
            FT_Done_Face(face);
            return;
        }
        context->files = files;
        context->file_capacity = capacity;
    }
    DFontFile *file = &context->files[context->file_count++];
    file->path = copy_string(path);
    file->family = copy_string(family);
    file->style = copy_string(style);
    file->weight = style_weight(style, face->style_flags);
    file->italic = style_italic(style, face->style_flags);
    file->monospace = (face->face_flags & FT_FACE_FLAG_FIXED_WIDTH) != 0;
    FT_Done_Face(face);
}

static void scan_font_directory(DRenderContext *context, const char *directory, int depth);

typedef struct {
    DRenderContext *context;
    const char *directory;
    int depth;
} FontScan;

static bool font_entry(void *opaque, const char *name, bool is_directory)
{
    FontScan *scan = opaque;
    if (name[0] == '.') return true;
    char *path = d_cat3(scan->directory, "/", name);
    if (is_directory) scan_font_directory(scan->context, path, scan->depth + 1);
    else if (has_font_extension(name)) add_font_file(scan->context, path);
    free(path);
    return true;
}

/* Recursion bounded by depth: a font collection is at most a few levels. */
static void scan_font_directory(DRenderContext *context, const char *directory, int depth)
{
    if (!directory || depth > 3) return;
    FontScan scan = {context, directory, depth};
    d_fs_list(directory, font_entry, &scan);
}

static void load_fonts(DRenderContext *context, const char *font_dir)
{
    const char *configured = font_dir && *font_dir ? font_dir : getenv("DISTILL_FONT_DIR");
    if (configured && *configured) {
        scan_font_directory(context, configured, 0);
        return;
    }
    static const char *defaults[] = {
        "/usr/share/fonts",
        "/usr/local/share/fonts",
        "/usr/share/fonts/truetype",
        NULL
    };
    for (size_t i = 0; defaults[i]; ++i) scan_font_directory(context, defaults[i], 0);
}

static void trim_family_token(const char **begin, const char **end)
{
    while (*begin < *end && isspace((unsigned char)**begin)) ++*begin;
    while (*end > *begin && isspace((unsigned char)*((*end) - 1))) --*end;
    if (*end - *begin >= 2 && ((*(*begin) == '\'' && *((*end) - 1) == '\'') ||
                               (*(*begin) == '"' && *((*end) - 1) == '"'))) {
        ++*begin;
        --*end;
        while (*begin < *end && isspace((unsigned char)**begin)) ++*begin;
        while (*end > *begin && isspace((unsigned char)*((*end) - 1))) --*end;
    }
}

static bool family_token_is(const char *begin, const char *end, const char *value)
{
    trim_family_token(&begin, &end);
    size_t length = (size_t)(end - begin);
    return strlen(value) == length && d_ncasecmp(begin, value, length) == 0;
}

static bool family_token_contains(const char *begin, const char *end, const char *value)
{
    trim_family_token(&begin, &end);
    size_t value_length = strlen(value);
    if (!value_length || (size_t)(end - begin) < value_length) return false;
    for (const char *cursor = begin; cursor + value_length <= end; ++cursor) {
        if (d_ncasecmp(cursor, value, value_length) == 0) return true;
    }
    return false;
}

static int family_token_score(const char *begin, const char *end, const DFontFile *file)
{
    trim_family_token(&begin, &end);
    if (begin == end) return -1;
    if (family_token_is(begin, end, file->family)) return 0;
    if (family_token_is(begin, end, "serif") &&
        family_token_contains(file->family, file->family + strlen(file->family), "serif")) return 20;
    if ((family_token_is(begin, end, "sans-serif") ||
         family_token_is(begin, end, "sans") ||
         family_token_is(begin, end, "system-ui")) &&
        family_token_contains(file->family, file->family + strlen(file->family), "sans")) return 20;
    if ((family_token_is(begin, end, "monospace") || family_token_is(begin, end, "mono")) &&
        file->monospace) return 20;
    return 1000;
}

/* litehtml passes the complete CSS font-family list to the container. Keep
   the order from that list: an exact family should beat a generic family,
   while a generic family should still be a useful fallback when the bundled
   face is unavailable. Quoted names are kept intact while finding commas. */
static int family_score(const char *requested, const DFontFile *file)
{
    if (!requested || !*requested) return 1000;
    int best = INT_MAX;
    const char *token_begin = requested;
    const char *cursor = requested;
    char quote = 0;
    unsigned int rank = 0;
    for (;;) {
        if (*cursor == '\'' || *cursor == '"') {
            if (quote == *cursor) quote = 0;
            else if (!quote) quote = *cursor;
        }
        if ((*cursor == ',' && !quote) || *cursor == '\0') {
            int token_score = family_token_score(token_begin, cursor, file);
            if (token_score >= 0) {
                int score = token_score >= 1000 ? 1000 + (int)rank * 100
                                                : token_score + (int)rank * 100;
                if (score < best) best = score;
            }
            if (*cursor == '\0') break;
            token_begin = cursor + 1;
            ++rank;
        }
        ++cursor;
    }
    return best == INT_MAX ? 1000 : best;
}

static const DFontFile *select_font_file(DRenderContext *context, const char *requested,
                                         int weight, bool italic)
{
    const DFontFile *best = NULL;
    int best_score = INT_MAX;
    for (size_t i = 0; i < context->file_count; ++i) {
        const DFontFile *file = &context->files[i];
        int score = family_score(requested, file);
        score += abs(file->weight - (weight >= 600 ? 700 : 400)) / 25;
        if (file->italic != italic) score += 8;
        if (score < best_score) {
            best = file;
            best_score = score;
        }
    }
    return best;
}

DRenderContext *d_render_create(int width, int height, const char *font_dir)
{
    if (width <= 0 || height <= 0) return NULL;
    DRenderContext *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    context->width = width;
    context->height = height;
    context->paint = d_paint_create(width, height);
    if (!context->paint || FT_Init_FreeType(&context->freetype) != 0) {
        d_render_destroy(context);
        return NULL;
    }
    load_fonts(context, font_dir);
    return context;
}

void d_render_destroy(DRenderContext *context)
{
    if (!context) return;
    for (size_t i = 0; i < context->file_count; ++i) {
        free(context->files[i].path);
        free(context->files[i].family);
        free(context->files[i].style);
    }
    free(context->files);
    if (context->freetype) FT_Done_FreeType(context->freetype);
    d_paint_destroy(context->paint);
    free(context);
}

int d_render_write_png(DRenderContext *context, const char *path)
{
    if (!context || !path) return 0;
    return d_paint_write_png(context->paint, path);
}

void d_render_clear(DRenderContext *context)
{
    if (!context) return;
    d_paint_clear(context->paint);
}

bool d_render_pixels(DRenderContext *context, DRenderPixels *pixels)
{
    return context && d_paint_pixels(context->paint, pixels);
}

DRenderFont *d_render_font_create(DRenderContext *context, const char *face_name,
                                  int size, int weight, bool italic,
                                  unsigned int decoration, DRenderFontMetrics *metrics)
{
    if (!context || size <= 0) return NULL;
    const DFontFile *file = select_font_file(context, face_name, weight, italic);
    if (!file) return NULL;
    DRenderFont *font = calloc(1, sizeof(*font));
    if (!font || FT_New_Face(context->freetype, file->path, 0, &font->face) != 0) {
        free(font);
        return NULL;
    }
    if (FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)size) != 0) {
        FT_Done_Face(font->face);
        free(font);
        return NULL;
    }
    font->harfbuzz = hb_ft_font_create_referenced(font->face);
    if (!font->harfbuzz) {
        FT_Done_Face(font->face);
        free(font);
        return NULL;
    }
    font->context = context;
    font->size = size;
    font->ascent = (int)((font->face->size->metrics.ascender + 32) / 64);
    font->descent = (int)((-font->face->size->metrics.descender + 32) / 64);
    font->height = (int)((font->face->size->metrics.height + 32) / 64);
    if (font->height < font->ascent + font->descent) font->height = font->ascent + font->descent;
    if (FT_Load_Char(font->face, 'x', FT_LOAD_DEFAULT) == 0)
        font->x_height = (int)((font->face->glyph->metrics.height + 32) / 64);
    else
        font->x_height = font->height / 2;
    font->decoration = decoration;
    if (metrics) {
        metrics->height = font->height;
        metrics->ascent = font->ascent;
        metrics->descent = font->descent;
        metrics->x_height = font->x_height;
        metrics->draw_spaces = true;
    }
    return font;
}

void d_render_font_destroy(DRenderFont *font)
{
    if (!font) return;
    if (font->harfbuzz) hb_font_destroy(font->harfbuzz);
    if (font->face) FT_Done_Face(font->face);
    free(font);
}

static hb_glyph_info_t *shape_text(DRenderFont *font, const char *text,
                                   hb_glyph_position_t **positions, unsigned int *count)
{
    hb_buffer_t *buffer = hb_buffer_create();
    if (!buffer) return NULL;
    hb_buffer_add_utf8(buffer, text ? text : "", -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font->harfbuzz, buffer, NULL, 0);
    *count = hb_buffer_get_length(buffer);
    unsigned int position_count = 0;
    hb_glyph_position_t *buffer_positions = hb_buffer_get_glyph_positions(buffer, &position_count);
    hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer, count);
    if (position_count != *count) {
        hb_buffer_destroy(buffer);
        return NULL;
    }
    size_t info_bytes = (size_t)*count * sizeof(*infos);
    size_t position_bytes = (size_t)*count * sizeof(*buffer_positions);
    hb_glyph_info_t *info_copy = malloc(info_bytes ? info_bytes : 1);
    *positions = malloc(position_bytes ? position_bytes : 1);
    if (!info_copy || !*positions) {
        free(info_copy);
        free(*positions);
        *positions = NULL;
        hb_buffer_destroy(buffer);
        return NULL;
    }
    if (info_bytes) memcpy(info_copy, infos, info_bytes);
    if (position_bytes) memcpy(*positions, buffer_positions, position_bytes);
    hb_buffer_destroy(buffer);
    return info_copy;
}

int d_render_text_width(DRenderFont *font, const char *text)
{
    if (!font || !text || !*text) return 0;
    hb_glyph_position_t *positions = NULL;
    unsigned int count = 0;
    hb_glyph_info_t *infos = shape_text(font, text, &positions, &count);
    if (!infos) return 0;
    int64_t width = 0;
    for (unsigned int i = 0; i < count; ++i) width += positions[i].x_advance;
    free(infos);
    free(positions);
    return (int)((width + 63) / 64);
}

/* One cluster at a time, because a cluster is what a reader can stand before
   or after: several glyphs can share one (a letter and the mark on it), and
   one glyph can cover several characters (an "fi" ligature). Walking glyphs
   instead would offer positions inside both. */
size_t d_render_text_offset_at(DRenderFont *font, const char *text, int x)
{
    if (!font || !text || !*text) return 0;
    size_t length = strlen(text);
    if (x <= 0) return 0;
    hb_glyph_position_t *positions = NULL;
    unsigned int count = 0;
    hb_glyph_info_t *infos = shape_text(font, text, &positions, &count);
    if (!infos) return 0;
    size_t offset = length;
    int64_t pen = 0;
    for (unsigned int i = 0; i < count;) {
        uint32_t cluster = infos[i].cluster;
        int64_t advance = 0;
        unsigned int next = i;
        while (next < count && infos[next].cluster == cluster)
            advance += positions[next++].x_advance;
        /* Past the middle of a cluster belongs to the far side of it, which is
           where every other text surface puts the caret. */
        if ((int64_t)x * 64 < pen + advance / 2) {
            offset = cluster;
            break;
        }
        pen += advance;
        i = next;
    }
    free(infos);
    free(positions);
    return offset > length ? length : offset;
}

int d_render_text_x_at(DRenderFont *font, const char *text, size_t offset)
{
    if (!font || !text || !*text || offset == 0) return 0;
    hb_glyph_position_t *positions = NULL;
    unsigned int count = 0;
    hb_glyph_info_t *infos = shape_text(font, text, &positions, &count);
    if (!infos) return 0;
    int64_t pen = 0;
    for (unsigned int i = 0; i < count; ++i)
        if (infos[i].cluster < offset) pen += positions[i].x_advance;
    free(infos);
    free(positions);
    return (int)((pen + 32) / 64);
}

static int measure_shaped_text(void *opaque, const char *text, size_t length)
{
    DRenderFont *font = opaque;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = '\0';
    int width = d_render_text_width(font, copy);
    free(copy);
    return width;
}

size_t d_render_wrap_text(DRenderFont *font, const char *text, int max_width,
                          DRenderTextLine *lines, size_t line_capacity)
{
    if (!font || !text || !lines) return 0;
    return d_text_wrap_utf8(text, strlen(text), max_width, measure_shaped_text, font,
                            (DTextLine *)lines, line_capacity);
}

static void draw_glyph(DRenderContext *context, DRenderFont *font, unsigned int glyph,
                       double x, double baseline, DRenderColor color)
{
    if (FT_Load_Glyph(font->face, glyph, FT_LOAD_DEFAULT) != 0) return;
    if (FT_Render_Glyph(font->face->glyph, FT_RENDER_MODE_NORMAL) != 0) return;
    FT_GlyphSlot slot = font->face->glyph;
    if (slot->bitmap.width == 0 || slot->bitmap.rows == 0 || slot->bitmap.pitch <= 0) return;
    d_paint_mask(context->paint, slot->bitmap.buffer, (int)slot->bitmap.width,
                 (int)slot->bitmap.rows, slot->bitmap.pitch,
                 x + slot->bitmap_left, baseline - slot->bitmap_top, color);
}

void d_render_draw_text(DRenderContext *context, const char *text, DRenderFont *font,
                        DRenderColor color, DRenderRect position)
{
    if (!context || !font || !text || !*text) return;
    hb_glyph_position_t *positions = NULL;
    unsigned int count = 0;
    hb_glyph_info_t *infos = shape_text(font, text, &positions, &count);
    if (!infos) return;
    double pen_x = position.x;
    double baseline = position.y + position.height - font->descent;
    for (unsigned int i = 0; i < count; ++i) {
        double x = pen_x + positions[i].x_offset / 64.0;
        double y = baseline - positions[i].y_offset / 64.0;
        draw_glyph(context, font, infos[i].codepoint, x, y, color);
        pen_x += positions[i].x_advance / 64.0;
    }
    if (font->decoration & 0x01u)
        d_paint_rule(context->paint, position.x, pen_x, baseline + 1.0, 1.0, color);
    if (font->decoration & 0x02u)
        d_paint_rule(context->paint, position.x, pen_x, baseline - font->size * 0.35, 1.0, color);
    free(infos);
    free(positions);
}

void d_render_fill(DRenderContext *context, DRenderRect rect, DRenderColor color)
{
    if (!context || !color.alpha) return;
    d_paint_fill(context->paint, rect, color);
}

/* The caller keeps its pixels: the painter reads them and is done with them
   before this returns. Scaling happens here rather than in the decoder,
   which stays ignorant of how large the image will be shown. */
void d_render_draw_image(DRenderContext *context, DRenderRect rect,
                         const uint32_t *pixels, int width, int height)
{
    if (!context || !pixels || width <= 0 || height <= 0) return;
    d_paint_image(context->paint, rect, pixels, width, height);
}

void d_render_stroke(DRenderContext *context, DRenderRect rect, int width,
                     DRenderColor color)
{
    if (!context || width <= 0 || !color.alpha) return;
    d_paint_stroke(context->paint, rect, width, color);
}

void d_render_draw_marker(DRenderContext *context, DRenderRect rect,
                          DRenderColor color, bool ordered, int index)
{
    if (!context) return;
    if (ordered) {
        char text[32];
        snprintf(text, sizeof(text), "%d.", index);
        DRenderFontMetrics metrics;
        DRenderFont *font = d_render_font_create(context, "sans-serif", 14, 400, false, 0, &metrics);
        if (font) {
            d_render_draw_text(context, text, font, color, rect);
            d_render_font_destroy(font);
        }
        return;
    }
    d_paint_disc(context->paint, rect.x + rect.width / 2.0, rect.y + rect.height / 2.0,
                 rect.width > 3 ? rect.width / 4.0 : 2.0, color);
}

void d_render_set_clip(DRenderContext *context, DRenderRect rect)
{
    if (!context) return;
    d_paint_clip_push(context->paint, rect);
    ++context->clip_depth;
}

void d_render_del_clip(DRenderContext *context)
{
    if (!context || !context->clip_depth) return;
    d_paint_clip_pop(context->paint);
    --context->clip_depth;
}
