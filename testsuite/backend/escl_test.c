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
    test_crop_full_surface_fallback();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
