#include "image.h"
#include "cutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <webp/decode.h>

static _Thread_local char image_error[192] = "";

static void set_error(const char *message)
{
    snprintf(image_error, sizeof(image_error), "%s", message ? message : "");
}

const char *d_image_last_error(void)
{
    return image_error;
}

static bool dimensions_ok(long width, long height)
{
    if (width <= 0 || height <= 0) { set_error("image has no area"); return false; }
    if (width > DISTILL_IMAGE_MAX_DIMENSION || height > DISTILL_IMAGE_MAX_DIMENSION) {
        set_error("image is larger than the decoder will open");
        return false;
    }
    if (width * height > DISTILL_IMAGE_MAX_PIXELS) {
        set_error("image is larger than the decoder will open");
        return false;
    }
    return true;
}

/* Straight alpha to premultiplied, in place, over a buffer already laid out
   as 0xAARRGGBB. A fully opaque image is left alone, which is the common case
   and the one worth not touching. */
static void premultiply(uint32_t *pixels, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint32_t pixel = pixels[i];
        uint32_t alpha = pixel >> 24;
        if (alpha == 255) continue;
        if (alpha == 0) { pixels[i] = 0; continue; }
        uint32_t red = (pixel >> 16) & 0xff, green = (pixel >> 8) & 0xff, blue = pixel & 0xff;
        red = (red * alpha + 127) / 255;
        green = (green * alpha + 127) / 255;
        blue = (blue * alpha + 127) / 255;
        pixels[i] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
}

/* Component bytes to the packed word the paint layer expects. Written in
   terms of the uint32 value rather than byte offsets so the result does not
   depend on the machine's endianness. */
static inline uint32_t pack(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    return ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
}

static bool starts_with(const unsigned char *bytes, size_t length,
                        const char *magic, size_t magic_length)
{
    return length >= magic_length && memcmp(bytes, magic, magic_length) == 0;
}

const char *d_image_format_name(const unsigned char *bytes, size_t length)
{
    if (!bytes) return "";
    if (starts_with(bytes, length, "\x89PNG\r\n\x1a\n", 8)) return "png";
    if (length >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) return "jpeg";
    if (length >= 12 && memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WEBP", 4) == 0) return "webp";
    if (starts_with(bytes, length, "GIF87a", 6) || starts_with(bytes, length, "GIF89a", 6)) return "gif";
    /* SVG is markup, and markup may open with a declaration, a comment or the
       element itself, so look for the element inside the opening bytes. */
    if (length >= 5) {
        size_t window = length < 512 ? length : 512;
        for (size_t i = 0; i + 4 < window; ++i)
            if (memcmp(bytes + i, "<svg", 4) == 0) return "svg";
    }
    return "";
}

static bool decode_png(const unsigned char *bytes, size_t length, DImage *out)
{
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes, length)) {
        set_error(image.message[0] ? image.message : "not a readable PNG");
        return false;
    }
    if (!dimensions_ok((long)image.width, (long)image.height)) {
        png_image_free(&image);
        return false;
    }
    /* PNG_FORMAT_RGBA is component order, not word order, so the rows are
       read as bytes and packed rather than cast. */
    image.format = PNG_FORMAT_RGBA;
    size_t count = (size_t)image.width * (size_t)image.height;
    png_bytep rows = malloc(count * 4);
    uint32_t *pixels = malloc(count * sizeof(*pixels));
    if (!rows || !pixels) {
        free(rows); free(pixels);
        png_image_free(&image);
        set_error("out of memory decoding a PNG");
        return false;
    }
    if (!png_image_finish_read(&image, NULL, rows, 0, NULL)) {
        set_error(image.message[0] ? image.message : "truncated PNG");
        free(rows); free(pixels);
        png_image_free(&image);
        return false;
    }
    for (size_t i = 0; i < count; ++i)
        pixels[i] = pack(rows[i * 4], rows[i * 4 + 1], rows[i * 4 + 2], rows[i * 4 + 3]);
    free(rows);
    premultiply(pixels, count);
    out->width = (int)image.width;
    out->height = (int)image.height;
    out->pixels = pixels;
    png_image_free(&image);
    return true;
}

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf escape;
    char message[JMSG_LENGTH_MAX];
} JpegError;

static void jpeg_fail(j_common_ptr info)
{
    JpegError *error = (JpegError *)info->err;
    (*info->err->format_message)(info, error->message);
    longjmp(error->escape, 1);
}

static bool decode_jpeg(const unsigned char *bytes, size_t length, DImage *out)
{
    struct jpeg_decompress_struct info;
    JpegError error;
    uint32_t *pixels = NULL;
    unsigned char *row = NULL;
    info.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_fail;
    error.message[0] = 0;
    if (setjmp(error.escape)) {
        set_error(error.message[0] ? error.message : "not a readable JPEG");
        jpeg_destroy_decompress(&info);
        free(pixels); free(row);
        return false;
    }
    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, bytes, (unsigned long)length);
    jpeg_read_header(&info, TRUE);
    info.out_color_space = JCS_RGB;
    jpeg_start_decompress(&info);
    if (!dimensions_ok((long)info.output_width, (long)info.output_height)) {
        jpeg_destroy_decompress(&info);
        return false;
    }
    size_t width = info.output_width, height = info.output_height;
    pixels = malloc(width * height * sizeof(*pixels));
    row = malloc(width * (size_t)info.output_components);
    if (!pixels || !row) {
        set_error("out of memory decoding a JPEG");
        jpeg_destroy_decompress(&info);
        free(pixels); free(row);
        return false;
    }
    for (size_t y = 0; y < height; ++y) {
        unsigned char *lines[1] = {row};
        jpeg_read_scanlines(&info, lines, 1);
        /* JPEG carries no alpha, so the rows are opaque and need no
           premultiplication pass. */
        if (info.output_components == 1)
            for (size_t x = 0; x < width; ++x)
                pixels[y * width + x] = pack(row[x], row[x], row[x], 255);
        else
            for (size_t x = 0; x < width; ++x)
                pixels[y * width + x] = pack(row[x * 3], row[x * 3 + 1], row[x * 3 + 2], 255);
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    free(row);
    out->width = (int)width;
    out->height = (int)height;
    out->pixels = pixels;
    return true;
}

static bool decode_webp(const unsigned char *bytes, size_t length, DImage *out)
{
    int width = 0, height = 0;
    if (!WebPGetInfo(bytes, length, &width, &height)) {
        set_error("not a readable WebP");
        return false;
    }
    if (!dimensions_ok(width, height)) return false;
    uint8_t *rgba = WebPDecodeRGBA(bytes, length, &width, &height);
    if (!rgba) { set_error("truncated WebP"); return false; }
    size_t count = (size_t)width * (size_t)height;
    uint32_t *pixels = malloc(count * sizeof(*pixels));
    if (!pixels) {
        WebPFree(rgba);
        set_error("out of memory decoding a WebP");
        return false;
    }
    for (size_t i = 0; i < count; ++i)
        pixels[i] = pack(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]);
    WebPFree(rgba);
    premultiply(pixels, count);
    out->width = width;
    out->height = height;
    out->pixels = pixels;
    return true;
}

bool d_image_decode(const unsigned char *bytes, size_t length, DImage *out)
{
    if (!out) return false;
    *out = (DImage){0};
    if (!bytes || !length) { set_error("no image data"); return false; }
    set_error("");
    const char *format = d_image_format_name(bytes, length);
    if (!strcmp(format, "png")) return decode_png(bytes, length, out);
    if (!strcmp(format, "jpeg")) return decode_jpeg(bytes, length, out);
    if (!strcmp(format, "webp")) return decode_webp(bytes, length, out);
    /* Naming the format is the useful half of the failure: "no GIF decoder"
       is something a reader can act on, an empty box is not. */
    if (*format) {
        char message[64];
        snprintf(message, sizeof(message), "no decoder for %s images", format);
        set_error(message);
    } else {
        set_error("unrecognised image format");
    }
    return false;
}

bool d_image_load_file(const char *path, DImage *out)
{
    if (out) *out = (DImage){0};
    if (!path || !*path) { set_error("no image path"); return false; }
    FILE *file = d_fopen(path, "rb");
    if (!file) { set_error("cannot open the image file"); return false; }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); set_error("cannot read the image file"); return false; }
    long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        set_error(size == 0 ? "the image file is empty" : "cannot read the image file");
        return false;
    }
    unsigned char *bytes = malloc((size_t)size);
    if (!bytes) { fclose(file); set_error("out of memory reading an image"); return false; }
    size_t read = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    bool ok = read == (size_t)size && d_image_decode(bytes, read, out);
    if (read != (size_t)size) set_error("the image file ended early");
    free(bytes);
    return ok;
}

void d_image_free(DImage *image)
{
    if (!image) return;
    free(image->pixels);
    *image = (DImage){0};
}
