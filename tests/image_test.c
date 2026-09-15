/* The decoder's contract, checked against real encoder output rather than
   against fixture files: the same three libraries that decode can encode, so
   each case is a round trip and there is nothing in the repository to go
   stale. What is actually being asserted is the part the paint layer depends
   on -- 0xAARRGGBB words, premultiplied -- which is easy to get subtly wrong
   and invisible until something is drawn. */
#include "image.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* jpeglib.h uses FILE without including stdio itself, so it has to follow. */
#include <jpeglib.h>
#include <png.h>
#include <webp/encode.h>

static uint8_t channel(uint32_t pixel, int shift) { return (uint8_t)((pixel >> shift) & 0xff); }

static void expect_pixel(uint32_t pixel, uint8_t alpha, uint8_t red, uint8_t green, uint8_t blue)
{
    assert(channel(pixel, 24) == alpha);
    assert(channel(pixel, 16) == red);
    assert(channel(pixel, 8) == green);
    assert(channel(pixel, 0) == blue);
}

/* Four pixels, one of them half-transparent, so premultiplication has
   something to do and the opaque fast path is exercised too. */
static const uint8_t source_rgba[4][4] = {
    {255, 0, 0, 255},      /* opaque red */
    {0, 255, 0, 255},      /* opaque green */
    {0, 0, 255, 128},      /* half-transparent blue */
    {255, 255, 255, 0},    /* fully transparent */
};

static void test_png_round_trip(void)
{
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = 4;
    image.height = 1;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    assert(png_image_write_to_memory(&image, NULL, &size, 0, source_rgba, 0, NULL));
    void *bytes = malloc(size);
    assert(bytes);
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = 4;
    image.height = 1;
    image.format = PNG_FORMAT_RGBA;
    assert(png_image_write_to_memory(&image, bytes, &size, 0, source_rgba, 0, NULL));

    assert(strcmp(d_image_format_name(bytes, size), "png") == 0);

    DImage decoded;
    assert(d_image_decode(bytes, size, &decoded));
    assert(decoded.width == 4 && decoded.height == 1);
    assert(decoded.pixels);
    expect_pixel(decoded.pixels[0], 255, 255, 0, 0);
    expect_pixel(decoded.pixels[1], 255, 0, 255, 0);
    /* 255 * 128 / 255 rounds to 128: the blue channel is scaled by alpha, and
       the channels that were zero stay zero. */
    expect_pixel(decoded.pixels[2], 128, 0, 0, 128);
    /* Zero alpha collapses to a zero word rather than keeping white, because
       premultiplied white at zero alpha is not white. */
    assert(decoded.pixels[3] == 0);
    d_image_free(&decoded);
    assert(decoded.pixels == NULL && decoded.width == 0);
    free(bytes);
}

static void test_jpeg_round_trip(void)
{
    struct jpeg_compress_struct info;
    struct jpeg_error_mgr error;
    unsigned char *bytes = NULL;
    unsigned long size = 0;
    info.err = jpeg_std_error(&error);
    jpeg_create_compress(&info);
    jpeg_mem_dest(&info, &bytes, &size);
    info.image_width = 8;
    info.image_height = 2;
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, 100, TRUE);
    jpeg_start_compress(&info, TRUE);
    unsigned char row[8 * 3];
    for (int x = 0; x < 8; ++x) { row[x * 3] = 200; row[x * 3 + 1] = 40; row[x * 3 + 2] = 40; }
    while (info.next_scanline < info.image_height) {
        unsigned char *lines[1] = {row};
        jpeg_write_scanlines(&info, lines, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);

    assert(strcmp(d_image_format_name(bytes, size), "jpeg") == 0);

    DImage decoded;
    assert(d_image_decode(bytes, size, &decoded));
    assert(decoded.width == 8 && decoded.height == 2);
    /* JPEG is lossy, so the assertion is that the image survived as the
       colour it was, not that every channel is identical. */
    for (int i = 0; i < 16; ++i) {
        assert(channel(decoded.pixels[i], 24) == 255);
        assert(channel(decoded.pixels[i], 16) > 150);
        assert(channel(decoded.pixels[i], 8) < 90);
        assert(channel(decoded.pixels[i], 0) < 90);
    }
    d_image_free(&decoded);
    free(bytes);
}

static void test_webp_round_trip(void)
{
    uint8_t rgba[4 * 4];
    memcpy(rgba, source_rgba, sizeof(rgba));
    uint8_t *bytes = NULL;
    size_t size = WebPEncodeLosslessRGBA(rgba, 4, 1, 4 * 4, &bytes);
    assert(size > 0 && bytes);

    assert(strcmp(d_image_format_name(bytes, size), "webp") == 0);

    DImage decoded;
    assert(d_image_decode(bytes, size, &decoded));
    assert(decoded.width == 4 && decoded.height == 1);
    expect_pixel(decoded.pixels[0], 255, 255, 0, 0);
    expect_pixel(decoded.pixels[1], 255, 0, 255, 0);
    expect_pixel(decoded.pixels[2], 128, 0, 0, 128);
    d_image_free(&decoded);
    WebPFree(bytes);
}

static void test_format_sniffing(void)
{
    /* The format comes from the bytes, never from a name or a served content
       type: both are page controlled, and a decoder pointed at the wrong
       parser by a lying header is the classic way into one. */
    assert(strcmp(d_image_format_name((const unsigned char *)"GIF89a....", 10), "gif") == 0);
    assert(strcmp(d_image_format_name((const unsigned char *)"<?xml ?><svg x>", 15), "svg") == 0);
    assert(strcmp(d_image_format_name((const unsigned char *)"not an image at all", 19), "") == 0);
    assert(strcmp(d_image_format_name(NULL, 0), "") == 0);
}

static void test_failures_are_reported(void)
{
    DImage decoded;
    assert(!d_image_decode(NULL, 0, &decoded));
    assert(!d_image_decode((const unsigned char *)"", 0, &decoded));

    /* A recognised format with no decoder must say which format it is. An
       empty box tells the reader nothing about why. */
    assert(!d_image_decode((const unsigned char *)"GIF89a\0\0\0\0", 10, &decoded));
    assert(strstr(d_image_last_error(), "gif"));
    assert(decoded.pixels == NULL);

    assert(!d_image_decode((const unsigned char *)"nonsense bytes here", 19, &decoded));
    assert(*d_image_last_error());

    /* Truncation must fail rather than decode whatever happens to follow. */
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = 4;
    image.height = 1;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    assert(png_image_write_to_memory(&image, NULL, &size, 0, source_rgba, 0, NULL));
    void *bytes = malloc(size);
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = 4;
    image.height = 1;
    image.format = PNG_FORMAT_RGBA;
    assert(png_image_write_to_memory(&image, bytes, &size, 0, source_rgba, 0, NULL));
    assert(!d_image_decode(bytes, size / 2, &decoded));
    assert(decoded.pixels == NULL);
    free(bytes);

    assert(!d_image_load_file("/nonexistent/path/to/an/image.png", &decoded));
    assert(!d_image_load_file(NULL, &decoded));
}

int main(void)
{
    test_png_round_trip();
    test_jpeg_round_trip();
    test_webp_round_trip();
    test_format_sniffing();
    test_failures_are_reported();
    printf("image: ok\n");
    return 0;
}
