/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 SANE Project

   This file is part of the SANE package and is distributed under the
   terms of the GNU General Public License version 3 or later.

   Unit tests for code paths which do not require a physical scanner. */

#define DEBUG_DECLARE_ONLY
#include "backend/escl/escl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
int sanei_debug_escl;

void
sanei_debug_escl_call(int level, const char *message, ...)
{
    (void)level;
    (void)message;
}

static void
expect_status(const char *name, SANE_Status actual, SANE_Status expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got %d, expected %d\n", name, actual, expected);
        failures++;
    }
}

static void
test_http_status(void)
{
    expect_status("HTTP 200", escl_http_status(200), SANE_STATUS_GOOD);
    expect_status("HTTP 204", escl_http_status(204), SANE_STATUS_GOOD);
    expect_status("HTTP 401", escl_http_status(401), SANE_STATUS_ACCESS_DENIED);
    expect_status("HTTP 409", escl_http_status(409), SANE_STATUS_NO_DOCS);
    expect_status("HTTP 503", escl_http_status(503), SANE_STATUS_DEVICE_BUSY);
    expect_status("HTTP 500", escl_http_status(500), SANE_STATUS_IO_ERROR);
}

static void
test_crop_passthrough(void)
{
    capabilities_t scanner = { 0 };
    unsigned char *surface = malloc(12);
    unsigned char *result;
    int width = 0;
    int height = 0;

    memset(surface, 7, 12);
    scanner.caps[PLATEN].width = 600;
    scanner.caps[PLATEN].height = 600;
    scanner.caps[PLATEN].default_resolution = 300;
    result = escl_crop_surface(&scanner, surface, 2, 2, 3, &width, &height);
    if (result != surface || width != 2 || height != 2 ||
        scanner.img_size != 12) {
        fprintf(stderr, "crop passthrough returned unexpected image\n");
        failures++;
    }
    free(result);
}

#if defined HAVE_LIBPNG
static void
test_png_grayscale_and_orientation(void)
{
    static const unsigned char png_data[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x00, 0x00, 0x00, 0x00, 0x57, 0xdd, 0x52,
        0xf8, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9c, 0x63, 0x10, 0x50, 0x60, 0x30,
        0x70, 0x00, 0x00, 0x01, 0x76, 0x00, 0xa1, 0xec,
        0x30, 0x8a, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    const unsigned char expected[] = {
        0x10, 0x10, 0x10, 0x20, 0x20, 0x20,
        0x30, 0x30, 0x30, 0x40, 0x40, 0x40
    };
    capabilities_t scanner = { 0 };
    int width = 0, height = 0, bps = 0;

    scanner.tmp = tmpfile();
    if (!scanner.tmp || fwrite(png_data, 1, sizeof(png_data), scanner.tmp) !=
                              sizeof(png_data)) {
        fprintf(stderr, "could not prepare PNG test image\n");
        if (scanner.tmp)
            fclose(scanner.tmp);
        failures++;
        return;
    }
    fseek(scanner.tmp, 0, SEEK_SET);
    if (get_PNG_data(&scanner, &width, &height, &bps) != SANE_STATUS_GOOD ||
        width != 2 || height != 2 || bps != 3 || scanner.img_size != 12 ||
        memcmp(scanner.img_data, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "PNG grayscale decoding returned unexpected pixels\n");
        failures++;
    }
    free(scanner.img_data);
    scanner.img_data = NULL;
}
#endif

static void
test_crop_full_surface_fallback(void)
{
    capabilities_t scanner = { 0 };
    unsigned char expected[] = {
        18, 19, 20, 21,
        26, 27, 28, 29,
        34, 35, 36, 37,
        42, 43, 44, 45
    };
    unsigned char *surface = malloc(64);
    unsigned char *result;
    int width = 0;
    int height = 0;

    for (int i = 0; i < 64; i++)
        surface[i] = (unsigned char)i;
    scanner.caps[PLATEN].width = 600;
    scanner.caps[PLATEN].height = 600;
    scanner.caps[PLATEN].pos_x = 300;
    scanner.caps[PLATEN].pos_y = 300;
    scanner.caps[PLATEN].MaxWidth = 1200;
    scanner.caps[PLATEN].MaxHeight = 1200;
    scanner.caps[PLATEN].default_resolution = 1;

    result = escl_crop_surface(&scanner, surface, 8, 8, 1, &width, &height);
    if (!result || width != 4 || height != 4 || scanner.img_size != 16 ||
        memcmp(result, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "fallback crop returned unexpected image\n");
        failures++;
    }
    free(result);
}

int
main(void)
{
    test_http_status();
    test_crop_passthrough();
#if defined HAVE_LIBPNG
    test_png_grayscale_and_orientation();
#endif
    test_crop_full_surface_fallback();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
