/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 Thierry HUCHARD

   This file implements banded PDF rendering for the eSCL backend.  */

#define DEBUG_DECLARE_ONLY
#include "../include/sane/config.h"

#include "escl.h"

#include <cairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if HAVE_POPPLER_GLIB
#include <poppler/glib/poppler.h>

struct pdf_stream
{
    FILE *file;
    GMappedFile *mapped;
    GBytes *bytes;
    PopplerDocument *document;
    PopplerPage *page;
    int width;
    int height;
    int x_off;
    int y_off;
    int real_width;
    int real_height;
    int band_height;
    double scale;
    int band_line;
    unsigned char *band;
    size_t row_size;
    size_t band_size;
    size_t band_pos;
    SANE_Bool eof;
};

static void
pdf_stream_dimensions(struct pdf_stream *stream, capabilities_t *scanner)
{
    caps_t *caps = &scanner->caps[scanner->source];
    int expected_w = (int)((double)caps->width * caps->default_resolution / 300.0 + 0.5);
    int expected_h = (int)((double)caps->height * caps->default_resolution / 300.0 + 0.5);

    stream->real_width = stream->width;
    stream->real_height = stream->height;
    if ((stream->width > expected_w + 4 || stream->height > expected_h + 4) &&
        caps->MaxWidth > 0 && caps->MaxHeight > 0) {
        double scale_x = (double)stream->width / caps->MaxWidth;
        double scale_y = (double)stream->height / caps->MaxHeight;
        stream->x_off = (int)(caps->pos_x * scale_x + 0.5);
        stream->y_off = (int)(caps->pos_y * scale_y + 0.5);
        stream->real_width = (int)(caps->width * scale_x + 0.5);
        stream->real_height = (int)(caps->height * scale_y + 0.5);
        if (stream->x_off < 0) stream->x_off = 0;
        if (stream->y_off < 0) stream->y_off = 0;
        if (stream->x_off >= stream->width) stream->real_width = 0;
        if (stream->y_off >= stream->height) stream->real_height = 0;
        if (stream->real_width > stream->width - stream->x_off)
            stream->real_width = stream->width - stream->x_off;
        if (stream->real_height > stream->height - stream->y_off)
            stream->real_height = stream->height - stream->y_off;
    }
}

static void
pdf_stream_free(struct pdf_stream *stream)
{
    if (!stream)
        return;
    if (stream->page)
        g_object_unref(stream->page);
    if (stream->document)
        g_object_unref(stream->document);
    if (stream->bytes)
        g_bytes_unref(stream->bytes);
    if (stream->mapped)
        g_mapped_file_unref(stream->mapped);
    if (stream->file)
        fclose(stream->file);
    free(stream->band);
    free(stream);
}

SANE_Status
escl_pdf_stream_start(capabilities_t *scanner,
                      int *width,
                      int *height,
                      int *bps)
{
    struct pdf_stream *stream;
    double page_width;
    double page_height;
    GError *error = NULL;

    if (!scanner || !scanner->tmp)
        return SANE_STATUS_INVAL;
    stream = calloc(1, sizeof(*stream));
    if (!stream)
        return SANE_STATUS_NO_MEM;
    stream->file = scanner->tmp;
    scanner->tmp = NULL;
    stream->mapped = g_mapped_file_new_from_fd(fileno(stream->file), FALSE, &error);
    if (!stream->mapped)
        goto error;
    stream->bytes = g_mapped_file_get_bytes(stream->mapped);
    if (!stream->bytes)
        goto error;
    stream->document = poppler_document_new_from_bytes(stream->bytes, NULL, &error);
    if (!stream->document)
        goto error;
    stream->page = poppler_document_get_page(stream->document, 0);
    if (!stream->page)
        goto error;
    poppler_page_get_size(stream->page, &page_width, &page_height);
    stream->width = (int)ceil(scanner->caps[scanner->source].default_resolution *
                               page_width / 72.0);
    stream->height = (int)ceil(scanner->caps[scanner->source].default_resolution *
                                page_height / 72.0);
    if (stream->width <= 0 || stream->height <= 0)
        goto error;
    stream->scale = scanner->caps[scanner->source].default_resolution / 72.0;
    pdf_stream_dimensions(stream, scanner);
    if (stream->real_width <= 0 || stream->real_height <= 0) {
        scanner->tmp = stream->file;
        stream->file = NULL;
        pdf_stream_free(stream);
        return SANE_STATUS_UNSUPPORTED;
    }
    stream->band_height = 64;
    stream->row_size = (size_t)stream->real_width * 3;
    stream->band_size = (size_t)stream->real_width * stream->band_height * 3;
    stream->band = malloc(stream->band_size);
    if (!stream->band)
        goto error;
    stream->band_size = 0;
    scanner->pdf_stream = stream;
    *width = stream->real_width;
    *height = stream->real_height;
    *bps = 3;
    if (error)
        g_error_free(error);
    return SANE_STATUS_GOOD;

error:
    if (error)
        g_error_free(error);
    pdf_stream_free(stream);
    return SANE_STATUS_INVAL;
}

static SANE_Status
pdf_stream_render_band(struct pdf_stream *stream)
{
    int remaining = stream->real_height - stream->band_line;
    int lines = remaining > stream->band_height ? stream->band_height : remaining;
    cairo_surface_t *surface;
    cairo_t *cr;
    unsigned char *data;
    int stride;

    if (lines <= 0) {
        stream->eof = SANE_TRUE;
        return SANE_STATUS_GOOD;
    }
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                          stream->width, lines);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return SANE_STATUS_NO_MEM;
    }
    cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, stream->scale, stream->scale);
    cairo_translate(cr, 0, -(stream->y_off + stream->band_line) / stream->scale);
    poppler_page_render(stream->page, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    data = cairo_image_surface_get_data(surface);
    stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < lines; y++) {
        unsigned int *src = (unsigned int *)(data + y * stride);
        unsigned char *dst = stream->band + (size_t)y * stream->row_size;
        for (int x = 0; x < stream->real_width; x++) {
            unsigned int pixel = src[stream->x_off + x];
            dst[x * 3] = (pixel >> 16) & 0xff;
            dst[x * 3 + 1] = (pixel >> 8) & 0xff;
            dst[x * 3 + 2] = pixel & 0xff;
        }
    }
    stream->band_size = (size_t)lines * stream->row_size;
    stream->band_pos = 0;
    cairo_surface_destroy(surface);
    return SANE_STATUS_GOOD;
}

SANE_Status
escl_pdf_stream_read(capabilities_t *scanner,
                     unsigned char *buf,
                     SANE_Int maxlen,
                     SANE_Int *len)
{
    struct pdf_stream *stream = scanner->pdf_stream;
    *len = 0;
    if (!stream || maxlen <= 0)
        return SANE_STATUS_INVAL;
    while (*len < maxlen && !stream->eof) {
        if (stream->band_pos >= stream->band_size) {
            SANE_Status status = pdf_stream_render_band(stream);
            if (status != SANE_STATUS_GOOD)
                return status;
            if (stream->eof)
                break;
            stream->band_line += stream->band_size / stream->row_size;
        }
        size_t count = stream->band_size - stream->band_pos;
        if (count > (size_t)(maxlen - *len))
            count = (size_t)(maxlen - *len);
        memcpy(buf + *len, stream->band + stream->band_pos, count);
        stream->band_pos += count;
        *len += (SANE_Int)count;
    }
    if (*len == 0 && stream->eof)
        return SANE_STATUS_EOF;
    return SANE_STATUS_GOOD;
}

void
escl_pdf_stream_finish(capabilities_t *scanner)
{
    struct pdf_stream *stream;
    if (!scanner || !scanner->pdf_stream)
        return;
    stream = scanner->pdf_stream;
    scanner->pdf_stream = NULL;
    pdf_stream_free(stream);
}

#else

SANE_Status
escl_pdf_stream_start(capabilities_t __sane_unused__ *scanner,
                      int __sane_unused__ *width,
                      int __sane_unused__ *height,
                      int __sane_unused__ *bps)
{
    return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
escl_pdf_stream_read(capabilities_t __sane_unused__ *scanner,
                     unsigned char __sane_unused__ *buf,
                     SANE_Int __sane_unused__ maxlen,
                     SANE_Int __sane_unused__ *len)
{
    return SANE_STATUS_UNSUPPORTED;
}

void
escl_pdf_stream_finish(capabilities_t __sane_unused__ *scanner)
{
}

#endif
