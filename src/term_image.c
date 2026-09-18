#define _POSIX_C_SOURCE 200809L

#include "term_image.h"

#include "cutil.h"

bool d_term_image_fit(const DImage *image, int box_width, int box_height,
                      uint32_t background, uint8_t **rgb, int *width, int *height)
{
    *rgb = NULL;
    *width = *height = 0;
    if (!image || !image->pixels || image->width <= 0 || image->height <= 0 ||
        box_width <= 0 || box_height <= 0)
        return false;
    double scale = 1.0;
    if (image->width > box_width) scale = (double)box_width / image->width;
    if (image->height * scale > box_height) scale = (double)box_height / image->height;
    int w = (int)(image->width * scale);
    int h = (int)(image->height * scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    uint8_t *out = d_xmalloc((size_t)w * (size_t)h * 3);
    unsigned br = (background >> 16) & 0xff, bg = (background >> 8) & 0xff, bb = background & 0xff;
    /* Box filter over the source pixels each output pixel covers: a
       downscale that samples one pixel in ten turns text in a screenshot
       into noise. */
    for (int y = 0; y < h; ++y) {
        int y0 = (int)((double)y / h * image->height), y1 = (int)((double)(y + 1) / h * image->height);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < w; ++x) {
            int x0 = (int)((double)x / w * image->width), x1 = (int)((double)(x + 1) / w * image->width);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long sr = 0, sg = 0, sb = 0, n = 0;
            for (int sy = y0; sy < y1 && sy < image->height; ++sy)
                for (int sx = x0; sx < x1 && sx < image->width; ++sx) {
                    uint32_t p = image->pixels[(size_t)sy * (size_t)image->width + (size_t)sx];
                    unsigned a = p >> 24;
                    /* Premultiplied: over the background is colour + bg*(1-a). */
                    sr += ((p >> 16) & 0xff) + br * (255 - a) / 255;
                    sg += ((p >> 8) & 0xff) + bg * (255 - a) / 255;
                    sb += (p & 0xff) + bb * (255 - a) / 255;
                    ++n;
                }
            uint8_t *o = out + ((size_t)y * (size_t)w + (size_t)x) * 3;
            o[0] = (uint8_t)(sr / n > 255 ? 255 : sr / n);
            o[1] = (uint8_t)(sg / n > 255 ? 255 : sg / n);
            o[2] = (uint8_t)(sb / n > 255 ? 255 : sb / n);
        }
    }
    *rgb = out;
    *width = w;
    *height = h;
    return true;
}

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64(const uint8_t *data, size_t length, size_t *out_length)
{
    size_t n = (length + 2) / 3 * 4;
    char *out = d_xmalloc(n + 1);
    size_t o = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < length) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < length) v |= data[i + 2];
        out[o++] = base64_alphabet[(v >> 18) & 63];
        out[o++] = base64_alphabet[(v >> 12) & 63];
        out[o++] = i + 1 < length ? base64_alphabet[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < length ? base64_alphabet[v & 63] : '=';
    }
    out[o] = '\0';
    *out_length = o;
    return out;
}

char *d_term_image_kitty(const uint8_t *rgb, int width, int height, int columns, int rows,
                         unsigned id)
{
    size_t encoded_length = 0;
    char *encoded = base64(rgb, (size_t)width * (size_t)height * 3, &encoded_length);
    DBuffer out;
    dbuf_init(&out);
    /* The payload goes in chunks of at most 4096 bytes; only the first
       carries the control data. q=2 asks the terminal to send no replies,
       which would otherwise arrive as input the shell has to discard. */
    const size_t chunk = 4096;
    for (size_t at = 0; at < encoded_length || at == 0; at += chunk) {
        bool more = at + chunk < encoded_length;
        if (at == 0)
            dbuf_printf(&out, "\033_Ga=T,f=24,s=%d,v=%d,c=%d,r=%d,i=%u,q=2,C=1,m=%d;",
                        width, height, columns, rows, id, more ? 1 : 0);
        else
            dbuf_printf(&out, "\033_Gm=%d;", more ? 1 : 0);
        size_t n = encoded_length - at < chunk ? encoded_length - at : chunk;
        dbuf_append(&out, encoded + at, n);
        dbuf_puts(&out, "\033\\");
        if (!more) break;
    }
    free(encoded);
    return dbuf_take(&out);
}

const char *d_term_image_kitty_clear(void)
{
    return "\033_Ga=d,d=A,q=2\033\\";
}

/* The palette: a 6x6x6 cube (216) and 24 grays. Fixed, so an image looks
   the same every time it is drawn and nothing is computed per image. */
static int palette_index(uint8_t r, uint8_t g, uint8_t b)
{
    int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (max - min < 12) {
        int gray = (r + g + b) / 3;
        return 216 + (gray * 23 + 127) / 255;
    }
    int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255, bi = (b * 5 + 127) / 255;
    return ri * 36 + gi * 6 + bi;
}

char *d_term_image_sixel(const uint8_t *rgb, int width, int height)
{
    DBuffer out;
    dbuf_init(&out);
    /* DCS P1=0 (aspect 2:1 default, overridden by raster attributes),
       P2=1 (pixels not drawn stay as they were), then "1;1 : square pixels. */
    dbuf_printf(&out, "\033P0;1;0q\"1;1;%d;%d", width, height);
    for (int i = 0; i < 216; ++i)
        dbuf_printf(&out, "#%d;2;%d;%d;%d", i, (i / 36) * 100 / 5, ((i / 6) % 6) * 100 / 5,
                    (i % 6) * 100 / 5);
    for (int i = 0; i < 24; ++i) {
        int level = i * 100 / 23;
        dbuf_printf(&out, "#%d;2;%d;%d;%d", 216 + i, level, level, level);
    }
    uint8_t *index = d_xmalloc((size_t)width * (size_t)height);
    for (size_t p = 0; p < (size_t)width * (size_t)height; ++p)
        index[p] = (uint8_t)palette_index(rgb[p * 3], rgb[p * 3 + 1], rgb[p * 3 + 2]);
    bool used[240];
    char *row = d_xmalloc((size_t)width);
    /* Six pixel rows at a time; each colour present in the band is one pass
       across it, and '$' returns to the band's start for the next. */
    for (int band = 0; band < height; band += 6) {
        memset(used, 0, sizeof(used));
        for (int y = band; y < band + 6 && y < height; ++y)
            for (int x = 0; x < width; ++x) used[index[(size_t)y * (size_t)width + (size_t)x]] = true;
        bool first = true;
        for (int colour = 0; colour < 240; ++colour) {
            if (!used[colour]) continue;
            if (!first) dbuf_puts(&out, "$");
            first = false;
            dbuf_printf(&out, "#%d", colour);
            for (int x = 0; x < width; ++x) {
                int bits = 0;
                for (int b = 0; b < 6 && band + b < height; ++b)
                    if (index[(size_t)(band + b) * (size_t)width + (size_t)x] == colour) bits |= 1 << b;
                row[x] = (char)(63 + bits);
            }
            /* Run-length: !N<char> for runs longer than three. */
            for (int x = 0; x < width;) {
                int run = 1;
                while (x + run < width && row[x + run] == row[x]) ++run;
                if (run > 3) dbuf_printf(&out, "!%d%c", run, row[x]);
                else for (int r = 0; r < run; ++r) dbuf_append(&out, &row[x], 1);
                x += run;
            }
        }
        dbuf_puts(&out, "-");
    }
    dbuf_puts(&out, "\033\\");
    free(row);
    free(index);
    return dbuf_take(&out);
}
