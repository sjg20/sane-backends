/* sane - Scanner Access Now Easy.

   Copyright (C) 2019 Touboul Nathane
   Copyright (C) 2019 Thierry HUCHARD <thierry@ordissimo.com>

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

#include "../include/sane/sanei.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

#if(defined HAVE_LIBPNG)
#include <png.h>
#endif

#include <setjmp.h>


#if(defined HAVE_LIBPNG)

/**
 * \fn SANE_Status escl_sane_decompressor(escl_sane_t *handler)
 * \brief Function that aims to decompress the png image to SANE be able to read the image.
 *        This function is called in the "sane_read" function.
 *
 * \return SANE_STATUS_GOOD (if everything is OK, otherwise, SANE_STATUS_NO_MEM/SANE_STATUS_INVAL)
 */
SANE_Status
get_PNG_data(capabilities_t *scanner, int *width, int *height, int *bps)
{
	png_byte magic[8];
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;
	volatile png_bytep *row_pointers = NULL;
	volatile unsigned char *surface = NULL;
	png_uint_32 w = 0, h = 0;
	size_t row_bytes;
	int bit_depth, color_type;
	volatile int surface_owned = 0;
	volatile SANE_Status status = SANE_STATUS_GOOD;

	if (!scanner || !scanner->tmp || !width || !height || !bps)
		return SANE_STATUS_INVAL;

	// read magic number
	if (fread(magic, 1, sizeof(magic), scanner->tmp) != sizeof(magic))
	{
		status = SANE_STATUS_INVAL;
		goto close_file;
	}
	// check for valid magic number
	if (!png_check_sig (magic, sizeof (magic)))
	{
		DBG( 10, "Escl Png : PNG error is not a valid PNG image!\n");
                status = SANE_STATUS_INVAL;
                goto close_file;
	}
	// create a png read struct
	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr)
	{
		DBG( 10, "Escl Png : PNG error create a png read struct\n");
			status = SANE_STATUS_NO_MEM;
                goto close_file;
	}
	// create a png info struct
	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		DBG( 10, "Escl Png : PNG error create a png info struct\n");
		status = SANE_STATUS_NO_MEM;
                goto close_file;
	}
	// initialize the setjmp for returning properly after a libpng
	//   error occurred
	if (setjmp (png_jmpbuf (png_ptr)))
	{
		DBG( 10, "Escl Png : PNG read error.\n");
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
		free((void *)row_pointers);
		free((void *)surface);
		if (scanner->tmp)
			fclose(scanner->tmp);
		scanner->tmp = NULL;
		return SANE_STATUS_INVAL;
	}
	// setup libpng for using standard C fread() function
	//   with our FILE pointer
	png_init_io (png_ptr, scanner->tmp);
	// tell libpng that we have already read the magic number
	png_set_sig_bytes (png_ptr, sizeof (magic));

	// read png info
	png_read_info (png_ptr, info_ptr);

	// get some useful information from header
	bit_depth = png_get_bit_depth (png_ptr, info_ptr);
	color_type = png_get_color_type (png_ptr, info_ptr);
	if (color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png_ptr);
	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png_ptr);
	if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png_ptr);
	if (color_type == PNG_COLOR_TYPE_RGB_ALPHA ||
	    color_type == PNG_COLOR_TYPE_GRAY_ALPHA ||
	    png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
		png_set_strip_alpha(png_ptr);
	if (bit_depth == 16)
		png_set_strip_16(png_ptr);
	else if (bit_depth < 8)
		png_set_packing(png_ptr);
	png_read_update_info(png_ptr, info_ptr);
	png_get_IHDR(png_ptr, info_ptr, &w, &h, &bit_depth, &color_type,
	             NULL, NULL, NULL);
	if (w == 0 || h == 0 || w > INT_MAX || h > INT_MAX ||
	    png_get_channels(png_ptr, info_ptr) != 3) {
		status = SANE_STATUS_INVAL;
		goto close_file;
	}
	row_bytes = png_get_rowbytes(png_ptr, info_ptr);
	if (row_bytes == 0 || (size_t)h > SIZE_MAX / row_bytes) {
		status = SANE_STATUS_NO_MEM;
		goto close_file;
	}
#if SIZE_MAX < UINT64_MAX
	if ((uint64_t)h > SIZE_MAX / sizeof(*row_pointers)) {
		status = SANE_STATUS_NO_MEM;
		goto close_file;
	}
#endif
	surface = malloc(row_bytes * (size_t)h);
	row_pointers = malloc(sizeof(*row_pointers) * (size_t)h);
	if (!surface || !row_pointers) {
		status = SANE_STATUS_NO_MEM;
		goto close_file;
	}
	for (png_uint_32 i = 0; i < h; i++)
		row_pointers[i] = (png_bytep)(uintptr_t)(surface +
		                                         (size_t)i * row_bytes);
	png_read_image(png_ptr, (png_bytepp)(uintptr_t)row_pointers);
	*width = (int)w;
	*height = (int)h;
	*bps = 3;
	surface = escl_crop_surface(scanner, (unsigned char *)surface,
                            (int)w, (int)h, 3,
	                            width, height);
	if (!surface)  {
		DBG(10, "Escl Png : Surface Memory allocation problem\n");
		status = SANE_STATUS_NO_MEM;
		goto close_file;
	}
	surface_owned = 1;

close_file:
	free((void *)row_pointers);
	if (!surface_owned)
		free((void *)surface);
	if (png_ptr)
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
	if (scanner->tmp)
        fclose(scanner->tmp);
    scanner->tmp = NULL;
    return (status);
}
#else

SANE_Status
get_PNG_data(capabilities_t __sane_unused__ *scanner,
              int __sane_unused__ *width,
              int __sane_unused__ *height,
              int __sane_unused__ *bps)
{
    return (SANE_STATUS_INVAL);
}

#endif
