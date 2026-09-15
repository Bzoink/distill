#define _POSIX_C_SOURCE 200809L

#include "render.h"
#include "shaper.h"

#include <cairo/cairo.h>
#include <ctype.h>
#include <dirent.h>
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
#include <strings.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    cairo_surface_t *surface;
    cairo_t *cairo;
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
    return strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0 ||
           strcasecmp(dot, ".ttc") == 0;
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    if (!text || !needle || !*needle) return false;
    size_t needle_length = strlen(needle);
    for (const char *cursor = text; *cursor; ++cursor) {
        if (strncasecmp(cursor, needle, needle_length) == 0) return true;
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

static void scan_font_directory(DRenderContext *context, const char *directory, int depth)
{
    if (!directory || depth > 3) return;
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        size_t path_length = strlen(directory) + strlen(entry->d_name) + 2;
        char *path = malloc(path_length);
        if (!path) continue;
        snprintf(path, path_length, "%s/%s", directory, entry->d_name);
        struct stat info;
        if (stat(path, &info) == 0) {
            if (S_ISDIR(info.st_mode)) scan_font_directory(context, path, depth + 1);
            else if (S_ISREG(info.st_mode) && has_font_extension(entry->d_name))
                add_font_file(context, path);
        }
        free(path);
    }
    closedir(dir);
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
    return strlen(value) == length && strncasecmp(begin, value, length) == 0;
}

static bool family_token_contains(const char *begin, const char *end, const char *value)
{
    trim_family_token(&begin, &end);
    size_t value_length = strlen(value);
    if (!value_length || (size_t)(end - begin) < value_length) return false;
    for (const char *cursor = begin; cursor + value_length <= end; ++cursor) {
        if (strncasecmp(cursor, value, value_length) == 0) return true;
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

static void set_color(cairo_t *cairo, DRenderColor color)
{
    cairo_set_source_rgba(cairo, color.red / 255.0, color.green / 255.0,
                          color.blue / 255.0, color.alpha / 255.0);
}

DRenderContext *d_render_create(int width, int height, const char *font_dir)
{
    if (width <= 0 || height <= 0) return NULL;
    DRenderContext *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    context->width = width;
    context->height = height;
    context->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    context->cairo = cairo_create(context->surface);
    if (cairo_status(context->cairo) != CAIRO_STATUS_SUCCESS ||
        FT_Init_FreeType(&context->freetype) != 0) {
        d_render_destroy(context);
        return NULL;
    }
    cairo_set_source_rgb(context->cairo, 0.96, 0.95, 0.92);
    cairo_paint(context->cairo);
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
    if (context->cairo) cairo_destroy(context->cairo);
    if (context->surface) cairo_surface_destroy(context->surface);
    free(context);
}

int d_render_write_png(DRenderContext *context, const char *path)
{
    if (!context || !path) return 0;
    cairo_surface_flush(context->surface);
    return cairo_surface_write_to_png(context->surface, path) == CAIRO_STATUS_SUCCESS;
}

void d_render_clear(DRenderContext *context)
{
    if (!context) return;
    cairo_save(context->cairo);
    cairo_set_source_rgb(context->cairo, 0.96, 0.95, 0.92);
    cairo_set_operator(context->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_paint(context->cairo);
    cairo_restore(context->cairo);
}

void *d_render_surface(DRenderContext *context)
{
    return context ? context->surface : NULL;
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
    if (slot->bitmap.width == 0 || slot->bitmap.rows == 0) return;
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8,
                                                        slot->bitmap.width, slot->bitmap.rows);
    if (cairo_surface_status(mask) != CAIRO_STATUS_SUCCESS) return;
    unsigned char *target = cairo_image_surface_get_data(mask);
    int stride = cairo_image_surface_get_stride(mask);
    for (unsigned int row = 0; row < slot->bitmap.rows; ++row) {
        const unsigned char *source = slot->bitmap.buffer + row * slot->bitmap.pitch;
        memcpy(target + row * stride, source, slot->bitmap.width);
    }
    cairo_surface_mark_dirty(mask);
    set_color(context->cairo, color);
    cairo_mask_surface(context->cairo, mask,
                       x + slot->bitmap_left,
                       baseline - slot->bitmap_top);
    cairo_surface_destroy(mask);
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
    cairo_save(context->cairo);
    for (unsigned int i = 0; i < count; ++i) {
        double x = pen_x + positions[i].x_offset / 64.0;
        double y = baseline - positions[i].y_offset / 64.0;
        draw_glyph(context, font, infos[i].codepoint, x, y, color);
        pen_x += positions[i].x_advance / 64.0;
    }
    if (font->decoration & 0x01u) {
        set_color(context->cairo, color);
        cairo_set_line_width(context->cairo, 1.0);
        cairo_move_to(context->cairo, position.x, baseline + 1.0);
        cairo_line_to(context->cairo, pen_x, baseline + 1.0);
        cairo_stroke(context->cairo);
    }
    if (font->decoration & 0x02u) {
        set_color(context->cairo, color);
        cairo_set_line_width(context->cairo, 1.0);
        cairo_move_to(context->cairo, position.x, baseline - font->size * 0.35);
        cairo_line_to(context->cairo, pen_x, baseline - font->size * 0.35);
        cairo_stroke(context->cairo);
    }
    cairo_restore(context->cairo);
    free(infos);
    free(positions);
}

void d_render_fill(DRenderContext *context, DRenderRect rect, DRenderColor color)
{
    if (!context || !color.alpha) return;
    cairo_save(context->cairo);
    set_color(context->cairo, color);
    cairo_rectangle(context->cairo, rect.x, rect.y, rect.width, rect.height);
    cairo_fill(context->cairo);
    cairo_restore(context->cairo);
}

/* Cairo reads the caller's buffer directly rather than taking a copy, so the
   surface is finished before this returns and the caller's pixels are free to
   go. Scaling happens on the pattern rather than by resampling the source,
   which keeps the decoder ignorant of how large the image will be shown. */
void d_render_draw_image(DRenderContext *context, DRenderRect rect,
                         const uint32_t *pixels, int width, int height)
{
    if (!context || !pixels || width <= 0 || height <= 0) return;
    if (rect.width <= 0 || rect.height <= 0) return;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    if (stride < 0) return;
    cairo_surface_t *image = NULL;
    unsigned char *packed = NULL;
    if (stride == width * 4) {
        image = cairo_image_surface_create_for_data((unsigned char *)pixels,
                                                    CAIRO_FORMAT_ARGB32,
                                                    width, height, stride);
    } else {
        /* Cairo rows are padded to its own alignment, which a tightly packed
           decode does not match at every width. */
        packed = malloc((size_t)stride * (size_t)height);
        if (!packed) return;
        for (int y = 0; y < height; ++y)
            memcpy(packed + (size_t)y * (size_t)stride, pixels + (size_t)y * (size_t)width,
                   (size_t)width * 4);
        image = cairo_image_surface_create_for_data(packed, CAIRO_FORMAT_ARGB32,
                                                    width, height, stride);
    }
    if (!image || cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
        if (image) cairo_surface_destroy(image);
        free(packed);
        return;
    }
    cairo_save(context->cairo);
    cairo_rectangle(context->cairo, rect.x, rect.y, rect.width, rect.height);
    cairo_clip(context->cairo);
    cairo_translate(context->cairo, rect.x, rect.y);
    cairo_scale(context->cairo, (double)rect.width / width, (double)rect.height / height);
    cairo_set_source_surface(context->cairo, image, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(context->cairo), CAIRO_FILTER_GOOD);
    cairo_paint(context->cairo);
    cairo_restore(context->cairo);
    cairo_surface_finish(image);
    cairo_surface_destroy(image);
    free(packed);
}

void d_render_stroke(DRenderContext *context, DRenderRect rect, int width,
                     DRenderColor color)
{
    if (!context || width <= 0 || !color.alpha) return;
    cairo_save(context->cairo);
    set_color(context->cairo, color);
    cairo_set_line_width(context->cairo, width);
    cairo_rectangle(context->cairo, rect.x + width / 2.0, rect.y + width / 2.0,
                    rect.width - width, rect.height - width);
    cairo_stroke(context->cairo);
    cairo_restore(context->cairo);
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
    cairo_save(context->cairo);
    set_color(context->cairo, color);
    cairo_arc(context->cairo, rect.x + rect.width / 2.0, rect.y + rect.height / 2.0,
              rect.width > 3 ? rect.width / 4.0 : 2.0, 0, 2 * M_PI);
    cairo_fill(context->cairo);
    cairo_restore(context->cairo);
}

void d_render_set_clip(DRenderContext *context, DRenderRect rect)
{
    if (!context) return;
    cairo_save(context->cairo);
    cairo_rectangle(context->cairo, rect.x, rect.y, rect.width, rect.height);
    cairo_clip(context->cairo);
    ++context->clip_depth;
}

void d_render_del_clip(DRenderContext *context)
{
    if (!context || !context->clip_depth) return;
    cairo_restore(context->cairo);
    --context->clip_depth;
}
