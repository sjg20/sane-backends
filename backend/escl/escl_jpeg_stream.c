/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 Thierry HUCHARD

   This file is part of the SANE package.

   SANE is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 2, or (at your option) any
   later version.

   This file implements progressive JPEG streaming for the eSCL backend.  */

#define DEBUG_DECLARE_ONLY
#include "../include/sane/config.h"

#include "escl.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(HAVE_LIBJPEG) && defined(USE_PTHREAD)
#include <jpeglib.h>

#define MAX_RETRIES 20
#define STREAM_BUFFER_SIZE 65536

struct stream_error_mgr
{
    struct jpeg_error_mgr errmgr;
    jmp_buf escape;
};

struct jpeg_stream
{
    const ESCL_Device *device;
    char *scanJob;
    char *result;
    pthread_t thread;
    SANE_Bool thread_started;
    pthread_mutex_t lock;
    int pipefd[2];
    int write_fd;
    SANE_Bool cancel;
    SANE_Status status;
    size_t real_read;
    FILE *input;
    struct jpeg_decompress_struct cinfo;
    struct stream_error_mgr jerr;
    SANE_Bool jpeg_created;
    SANE_Bool jpeg_started;
    unsigned char *row;
    size_t row_size;
    size_t row_pos;
    int width;
    int height;
    int bps;
    int x_off;
    int y_off;
    int real_width;
    int real_height;
    SANE_Bool eof;
    unsigned char input_buffer[4096];
};

static size_t
stream_write_callback(void *ptr, size_t size, size_t nmemb, void *data)
{
    struct jpeg_stream *stream = data;
    size_t total = size * nmemb;
    size_t written = 0;

    while (written < total) {
        pthread_mutex_lock(&stream->lock);
        int write_fd = stream->write_fd >= 0 ? dup(stream->write_fd) : -1;
        pthread_mutex_unlock(&stream->lock);
        if (write_fd < 0)
            return written;
        ssize_t count = write(write_fd,
                              (unsigned char *)ptr + written,
                              total - written);
        if (count <= 0)
        {
            close(write_fd);
            return written;
        }
        close(write_fd);
        written += (size_t)count;
        stream->real_read += (size_t)count;
    }
    return total;
}

static void
stream_close_write_fd(struct jpeg_stream *stream)
{
    pthread_mutex_lock(&stream->lock);
    if (stream->write_fd >= 0) {
        close(stream->write_fd);
        stream->write_fd = -1;
    }
    pthread_mutex_unlock(&stream->lock);
}

static void *
stream_worker(void *data)
{
    struct jpeg_stream *stream = data;
    const ESCL_Device *device = stream->device;
    char scan_cmd[PATH_MAX] = { 0 };
    CURL *curl_handle = NULL;

    snprintf(scan_cmd, sizeof(scan_cmd), "/eSCL/%s%s/NextDocument",
             stream->scanJob, stream->result);
    curl_handle = escl_curl_init(device, scan_cmd);
    if (!curl_handle) {
        stream->status = SANE_STATUS_NO_MEM;
        stream_close_write_fd(stream);
        return NULL;
    }
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, stream);

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        CURLcode result = curl_easy_perform(curl_handle);
        SANE_Status status = escl_curl_status(curl_handle, result);
        if (escl_curl_retry(status, attempt, MAX_RETRIES)) {
            DBG(10, "Scanner busy: reattempting scan (%d/%d)\n",
                attempt + 1, MAX_RETRIES);
            continue;
        }
        if (status != SANE_STATUS_GOOD)
            stream->status = status;
        else if (stream->real_read == 0)
            stream->status = SANE_STATUS_NO_DOCS;
        else
            stream->status = SANE_STATUS_GOOD;
        break;
    }
    curl_easy_cleanup(curl_handle);
    stream_close_write_fd(stream);
    return NULL;
}

static void
stream_init_source(j_decompress_ptr __sane_unused__ cinfo)
{
}

static boolean
stream_fill_input_buffer(j_decompress_ptr cinfo)
{
    struct jpeg_stream *stream = cinfo->client_data;
    FILE *input = stream->input;
    size_t count;

    count = fread(stream->input_buffer,
                  1, sizeof(stream->input_buffer),
                  input);
    if (count == 0) {
        stream->input_buffer[0] = 0xff;
        stream->input_buffer[1] = JPEG_EOI;
        count = 2;
    }
    cinfo->src->next_input_byte = stream->input_buffer;
    cinfo->src->bytes_in_buffer = count;
    return TRUE;
}

static void
stream_skip_input_data(j_decompress_ptr cinfo, long count)
{
    while (count > (long)cinfo->src->bytes_in_buffer) {
        count -= (long)cinfo->src->bytes_in_buffer;
        if (!stream_fill_input_buffer(cinfo))
            return;
    }
    cinfo->src->next_input_byte += count;
    cinfo->src->bytes_in_buffer -= (size_t)count;
}

static void
stream_term_source(j_decompress_ptr cinfo)
{
    (void)cinfo;
}

static void
stream_jpeg_source(j_decompress_ptr cinfo, struct jpeg_stream *stream)
{
    if (!cinfo->src)
        cinfo->src = (*cinfo->mem->alloc_small)((j_common_ptr)cinfo,
                                                 JPOOL_PERMANENT,
                                                 sizeof(struct jpeg_source_mgr));
    cinfo->src->init_source = stream_init_source;
    cinfo->src->fill_input_buffer = stream_fill_input_buffer;
    cinfo->src->skip_input_data = stream_skip_input_data;
    cinfo->src->resync_to_restart = jpeg_resync_to_restart;
    cinfo->src->term_source = stream_term_source;
    cinfo->src->bytes_in_buffer = 0;
    cinfo->src->next_input_byte = NULL;
    cinfo->client_data = stream;
}

static void
stream_error_exit(j_common_ptr cinfo)
{
    struct stream_error_mgr *jerr = (struct stream_error_mgr *)cinfo->err;
    longjmp(jerr->escape, 1);
}

static void
stream_output_message(j_common_ptr __sane_unused__ cinfo)
{
}

static void
stream_dimensions(struct jpeg_stream *stream, capabilities_t *scanner)
{
    caps_t *caps = &scanner->caps[scanner->source];
    int expected_w = (int)((double)caps->width * caps->default_resolution / 300.0 + 0.5);
    int expected_h = (int)((double)caps->height * caps->default_resolution / 300.0 + 0.5);

    stream->width = stream->cinfo.output_width;
    stream->height = stream->cinfo.output_height;
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

SANE_Status
escl_jpeg_stream_start(capabilities_t *scanner,
                       const ESCL_Device *device,
                       char *scanJob,
                       char *result,
                       int *width,
                       int *height,
                       int *bps)
{
    struct jpeg_stream *stream = calloc(1, sizeof(*stream));
    if (!stream)
        return SANE_STATUS_NO_MEM;
    stream->pipefd[0] = -1;
    stream->pipefd[1] = -1;
    stream->write_fd = -1;
    stream->status = SANE_STATUS_IO_ERROR;
    stream->device = device;
    stream->scanJob = scanJob;
    stream->result = result;
    pthread_mutex_init(&stream->lock, NULL);
    if (pipe(stream->pipefd) != 0)
        goto error;
    stream->write_fd = stream->pipefd[1];
    if (pthread_create(&stream->thread, NULL, stream_worker, stream) != 0)
        goto error;
    stream->thread_started = SANE_TRUE;
    stream->input = fdopen(stream->pipefd[0], "rb");
    if (!stream->input)
        goto error;
    stream->pipefd[0] = -1;
    stream->cinfo.err = jpeg_std_error(&stream->jerr.errmgr);
    stream->jerr.errmgr.error_exit = stream_error_exit;
    stream->jerr.errmgr.output_message = stream_output_message;
    if (setjmp(stream->jerr.escape))
        goto error;
    jpeg_create_decompress(&stream->cinfo);
    stream->jpeg_created = SANE_TRUE;
    stream_jpeg_source(&stream->cinfo, stream);
    jpeg_read_header(&stream->cinfo, TRUE);
    stream->cinfo.out_color_space = JCS_RGB;
    stream->cinfo.quantize_colors = FALSE;
    jpeg_calc_output_dimensions(&stream->cinfo);
    stream->bps = stream->cinfo.output_components;
    stream_dimensions(stream, scanner);
    stream->row_size = (size_t)stream->real_width * stream->bps;
    stream->row = stream->row_size ? malloc(stream->row_size) : NULL;
    if (!stream->row)
        goto error;
    stream->row_pos = stream->row_size;
    jpeg_start_decompress(&stream->cinfo);
    stream->jpeg_started = SANE_TRUE;
    scanner->jpeg_stream = stream;
    *width = stream->real_width;
    *height = stream->real_height;
    *bps = stream->bps;
    return SANE_STATUS_GOOD;

error:
    escl_jpeg_stream_finish((capabilities_t *)&(capabilities_t){ .jpeg_stream = stream });
    return SANE_STATUS_INVAL;
}

SANE_Status
escl_jpeg_stream_read(capabilities_t *scanner,
                      unsigned char *buf,
                      SANE_Int maxlen,
                      SANE_Int *len)
{
    struct jpeg_stream *stream = scanner->jpeg_stream;
    *len = 0;
    if (!stream || maxlen <= 0)
        return SANE_STATUS_INVAL;
    if (setjmp(stream->jerr.escape))
        return SANE_STATUS_INVAL;
    while (*len < maxlen && !stream->eof) {
        if (stream->row_pos < stream->row_size) {
            size_t count = stream->row_size - stream->row_pos;
            if (count > (size_t)(maxlen - *len))
                count = (size_t)(maxlen - *len);
            memcpy(buf + *len, stream->row + stream->row_pos, count);
            stream->row_pos += count;
            *len += (SANE_Int)count;
            continue;
        }
        if (stream->cinfo.output_scanline >= stream->cinfo.output_height) {
            jpeg_finish_decompress(&stream->cinfo);
            stream->eof = SANE_TRUE;
            break;
        }
        JSAMPROW row = stream->row;
        JDIMENSION line = stream->cinfo.output_scanline;
        unsigned char *full = malloc((size_t)stream->width * stream->bps);
        if (!full)
            return SANE_STATUS_NO_MEM;
        row = full;
        jpeg_read_scanlines(&stream->cinfo, &row, 1);
        stream->row_pos = 0;
        if (line < (JDIMENSION)stream->y_off ||
            line >= (JDIMENSION)(stream->y_off + stream->real_height)) {
            free(full);
            continue;
        }
        memcpy(stream->row, full + (size_t)stream->x_off * stream->bps,
               stream->row_size);
        free(full);
    }
    if (*len == 0 && stream->eof)
        return SANE_STATUS_EOF;
    return SANE_STATUS_GOOD;
}

void
escl_jpeg_stream_finish(capabilities_t *scanner)
{
    struct jpeg_stream *stream;
    if (!scanner || !scanner->jpeg_stream)
        return;
    stream = scanner->jpeg_stream;
    pthread_mutex_lock(&stream->lock);
    stream->cancel = SANE_TRUE;
    if (stream->write_fd >= 0) {
        close(stream->write_fd);
        stream->write_fd = -1;
    }
    pthread_mutex_unlock(&stream->lock);
    if (stream->thread_started)
        pthread_join(stream->thread, NULL);
    if (stream->jpeg_created)
        jpeg_destroy_decompress(&stream->cinfo);
    if (stream->input)
        fclose(stream->input);
    if (stream->pipefd[0] >= 0)
        close(stream->pipefd[0]);
    if (stream->pipefd[1] >= 0)
        close(stream->pipefd[1]);
    pthread_mutex_destroy(&stream->lock);
    free(stream->row);
    free(stream);
    scanner->jpeg_stream = NULL;
}

#else

SANE_Status
escl_jpeg_stream_start(capabilities_t __sane_unused__ *scanner,
                       const ESCL_Device __sane_unused__ *device,
                       char __sane_unused__ *scanJob,
                       char __sane_unused__ *result,
                       int __sane_unused__ *width,
                       int __sane_unused__ *height,
                       int __sane_unused__ *bps)
{
    return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
escl_jpeg_stream_read(capabilities_t __sane_unused__ *scanner,
                      unsigned char __sane_unused__ *buf,
                      SANE_Int __sane_unused__ maxlen,
                      SANE_Int __sane_unused__ *len)
{
    return SANE_STATUS_UNSUPPORTED;
}

void
escl_jpeg_stream_finish(capabilities_t __sane_unused__ *scanner)
{
}

#endif
