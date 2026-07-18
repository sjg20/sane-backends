/* sane - Scanner Access Now Easy.

   Copyright (C) 2020 Thierry HUCHARD <thierry@ordissimo.com>

   This file is part of the SANE package.

   SANE is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3 of the License, or (at your
   option) any later version.

   SANE is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   You should have received a copy of the GNU General Public License
   along with sane; see the file COPYING.
   If not, see <https://www.gnu.org/licenses/>.

   This file implements a SANE backend for eSCL scanners.  */

#define DEBUG_DECLARE_ONLY
#include "../include/sane/config.h"

#include "escl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

unsigned char *
escl_crop_surface(capabilities_t *scanner,
               unsigned char *surface,
	       int w,
	       int h,
	       int bps,
	       int *width,
	       int *height)
{
    double scale_x;
    double scale_y;
    int x_off = 0;
    int real_w = w;
    int y_off = 0;
    int real_h = h;
    unsigned char *surface_crop = NULL;
    size_t row_size;
    size_t output_size;

    DBG( 10, "Escl Image Crop\n");
    if (!scanner || !surface || !width || !height ||
        w <= 0 || h <= 0 || bps <= 0)
        return NULL;

    caps_t *caps = &scanner->caps[scanner->source];
    int expected_w = (int)ceil((double)caps->width *
                              caps->default_resolution / 300.0);
    int expected_h = (int)ceil((double)caps->height *
                              caps->default_resolution / 300.0);

    /* Most scanners honour ScanRegion.  Crop only when the returned image is
     * larger and therefore appears to contain the complete scan surface. */
    if ((w > expected_w + 4 || h > expected_h + 4) &&
        caps->MaxWidth > 0 && caps->MaxHeight > 0) {
        scale_x = (double)w / caps->MaxWidth;
        scale_y = (double)h / caps->MaxHeight;
        x_off = (int)lround(caps->pos_x * scale_x);
        y_off = (int)lround(caps->pos_y * scale_y);
        real_w = (int)lround(caps->width * scale_x);
        real_h = (int)lround(caps->height * scale_y);
        if (x_off < 0) x_off = 0;
        if (y_off < 0) y_off = 0;
        if (x_off >= w || y_off >= h) goto invalid_region;
        if (real_w > w - x_off) real_w = w - x_off;
        if (real_h > h - y_off) real_h = h - y_off;
    }

    DBG( 10, "Escl Image Crop [%dx%d|%dx%d]\n", scanner->caps[scanner->source].pos_x, scanner->caps[scanner->source].pos_y,
		    scanner->caps[scanner->source].width, scanner->caps[scanner->source].height);

    *width = real_w;
    *height = real_h;
    DBG( 10, "Escl Image Crop [%dx%d]\n", *width, *height);
    if (real_w <= 0 || real_h <= 0) goto invalid_region;
    row_size = (size_t)real_w * (size_t)bps;
    if ((size_t)real_h > (size_t)-1 / row_size) goto invalid_region;
    output_size = row_size * (size_t)real_h;
    if (x_off > 0 || real_w < w || y_off > 0 || real_h < h) {
          surface_crop = (unsigned char *)malloc(output_size);
	  if(!surface_crop) {
             DBG( 10, "Escl Crop : Surface_crop Memory allocation problem\n");
	     free(surface);
	     surface = NULL;
	     goto finish;
	  }
          for (int y = 0; y < real_h; y++)
             memcpy(surface_crop + (size_t)y * row_size,
                    surface + ((size_t)(y + y_off) * w + x_off) * bps,
                    row_size);
          free(surface);
	  surface = surface_crop;
    }
    // we don't need row pointers anymore
    scanner->img_data = surface;
    scanner->img_size = (long)output_size;
    scanner->img_read = 0;
finish:
    return surface;
invalid_region:
    free(surface);
    return NULL;
}
