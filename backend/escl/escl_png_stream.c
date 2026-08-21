/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 Thierry HUCHARD

   This file implements progressive PNG streaming for the eSCL backend.  */

#define DEBUG_DECLARE_ONLY
#include "../include/sane/config.h"

#include "escl.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(HAVE_LIBPNG) && defined(USE_PTHREAD)
#include <png.h>

#define MAX_RETRIES 20

struct png_stream
{
    const ESCL_Device *device;
    char *scanJob;
    char *result;
    pthread_t thread;
    SANE_Bool thread_started;
    pthread_mutex_t lock;
    int pipefd[2];
    int write_fd;
    FILE *input;
    size_t real_read;
    SANE_Status status;
    png_structp png_ptr;
    png_infop info_ptr;
    unsigned char *row;
    unsigned char input_buffer[4096];
    size_t row_size;
    size_t row_pos;
    int width;
    int height;
    int x_off;
    int y_off;
    int real_width;
    int real_height;
    int rows_read;
    SANE_Bool eof;
};

static size_t
png_stream_write(void *ptr, size_t size, size_t nmemb, void *data)
{
    struct png_stream *stream = data;
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
png_stream_close_write(struct png_stream *stream)
{
    pthread_mutex_lock(&stream->lock);
    if (stream->write_fd >= 0) {
        close(stream->write_fd);
        stream->write_fd = -1;
    }
    pthread_mutex_unlock(&stream->lock);
}

static void *
png_stream_worker(void *data)
{
    struct png_stream *stream = data;
    char scan_cmd[PATH_MAX] = { 0 };
    CURL *curl_handle;

    snprintf(scan_cmd, sizeof(scan_cmd), "/eSCL/%s%s/NextDocument",
             stream->scanJob, stream->result);
    curl_handle = escl_curl_init(stream->device, scan_cmd);
    if (!curl_handle) {
        stream->status = SANE_STATUS_NO_MEM;
        png_stream_close_write(stream);
        return NULL;
    }
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, png_stream_write);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, stream);
    stream->status = SANE_STATUS_IO_ERROR;
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        CURLcode result = curl_easy_perform(curl_handle);
        SANE_Status status = escl_curl_status(curl_handle, result);
        if (escl_curl_retry(status, attempt, MAX_RETRIES))
            continue;
        stream->status = status;
        if (status == SANE_STATUS_GOOD && stream->real_read == 0)
            stream->status = SANE_STATUS_NO_DOCS;
        break;
    }
    curl_easy_cleanup(curl_handle);
    png_stream_close_write(stream);
    return NULL;
}

static void
png_stream_read_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
    struct png_stream *stream = png_get_io_ptr(png_ptr);
    if (fread(data, 1, length, stream->input) != length)
        png_error(png_ptr, "short PNG input");
}

static void
png_stream_dimensions(struct png_stream *stream, capabilities_t *scanner)
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

SANE_Status
escl_png_stream_start(capabilities_t *scanner,
                      const ESCL_Device *device,
                      char *scanJob,
                      char *result,
                      int *width,
                      int *height,
                      int *bps)
{
    struct png_stream *stream = calloc(1, sizeof(*stream));
    png_byte signature[8];
    int bit_depth;
    int color_type;

    if (!stream)
        return SANE_STATUS_NO_MEM;
    stream->device = device;
    stream->scanJob = scanJob;
    stream->result = result;
    stream->pipefd[0] = -1;
    stream->pipefd[1] = -1;
    stream->write_fd = -1;
    pthread_mutex_init(&stream->lock, NULL);
    if (pipe(stream->pipefd) != 0)
        goto error;
    stream->write_fd = stream->pipefd[1];
    if (pthread_create(&stream->thread, NULL, png_stream_worker, stream) != 0)
        goto error;
    stream->thread_started = SANE_TRUE;
    stream->input = fdopen(stream->pipefd[0], "rb");
    if (!stream->input)
        goto error;
    stream->pipefd[0] = -1;
    if (fread(signature, 1, sizeof(signature), stream->input) != sizeof(signature) ||
        !png_check_sig(signature, sizeof(signature)))
        goto error;
    stream->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    stream->info_ptr = stream->png_ptr ? png_create_info_struct(stream->png_ptr) : NULL;
    if (!stream->png_ptr || !stream->info_ptr)
        goto error;
    if (setjmp(png_jmpbuf(stream->png_ptr)))
        goto error;
    png_set_read_fn(stream->png_ptr, stream, png_stream_read_data);
    png_set_sig_bytes(stream->png_ptr, sizeof(signature));
    png_read_info(stream->png_ptr, stream->info_ptr);
    bit_depth = png_get_bit_depth(stream->png_ptr, stream->info_ptr);
    color_type = png_get_color_type(stream->png_ptr, stream->info_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(stream->png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(stream->png_ptr);
    if (png_get_valid(stream->png_ptr, stream->info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(stream->png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB_ALPHA ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA ||
        png_get_valid(stream->png_ptr, stream->info_ptr, PNG_INFO_tRNS))
        png_set_strip_alpha(stream->png_ptr);
    if (bit_depth == 16)
        png_set_strip_16(stream->png_ptr);
    else if (bit_depth < 8)
        png_set_packing(stream->png_ptr);
    png_read_update_info(stream->png_ptr, stream->info_ptr);
    stream->width = (int)png_get_image_width(stream->png_ptr, stream->info_ptr);
    stream->height = (int)png_get_image_height(stream->png_ptr, stream->info_ptr);
    if (stream->width <= 0 || stream->height <= 0 ||
        png_get_channels(stream->png_ptr, stream->info_ptr) != 3)
        goto error;
    png_stream_dimensions(stream, scanner);
    stream->row_size = (size_t)stream->real_width * 3;
    stream->row = stream->row_size ? malloc(stream->row_size) : NULL;
    if (!stream->row)
        goto error;
    stream->row_pos = stream->row_size;
    scanner->png_stream = stream;
    *width = stream->real_width;
    *height = stream->real_height;
    *bps = 3;
    return SANE_STATUS_GOOD;

error:
    if (stream->thread_started)
        png_stream_close_write(stream);
    if (stream->thread_started)
        pthread_join(stream->thread, NULL);
    if (stream->png_ptr)
        png_destroy_read_struct(&stream->png_ptr, &stream->info_ptr, NULL);
    if (stream->input)
        fclose(stream->input);
    if (stream->pipefd[0] >= 0) close(stream->pipefd[0]);
    if (stream->pipefd[1] >= 0) close(stream->pipefd[1]);
    free(stream->row);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
    return SANE_STATUS_INVAL;
}

SANE_Status
escl_png_stream_read(capabilities_t *scanner,
                     unsigned char *buf,
                     SANE_Int maxlen,
                     SANE_Int *len)
{
    struct png_stream *stream = scanner->png_stream;
    *len = 0;
    if (!stream || maxlen <= 0)
        return SANE_STATUS_INVAL;
    if (setjmp(png_jmpbuf(stream->png_ptr)))
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
        if (stream->rows_read >= stream->height) {
            png_read_end(stream->png_ptr, stream->info_ptr);
            stream->eof = SANE_TRUE;
            break;
        }
        png_bytep full = malloc((size_t)stream->width * 3);
        if (!full)
            return SANE_STATUS_NO_MEM;
        png_read_row(stream->png_ptr, full, NULL);
        int line = stream->rows_read++;
        if (line >= stream->y_off && line < stream->y_off + stream->real_height) {
            memcpy(stream->row, full + (size_t)stream->x_off * 3, stream->row_size);
            stream->row_pos = 0;
        }
        free(full);
    }
    if (*len == 0 && stream->eof)
        return SANE_STATUS_EOF;
    return SANE_STATUS_GOOD;
}

void
escl_png_stream_finish(capabilities_t *scanner)
{
    struct png_stream *stream;
    if (!scanner || !scanner->png_stream)
        return;
    stream = scanner->png_stream;
    png_stream_close_write(stream);
    if (stream->thread_started)
        pthread_join(stream->thread, NULL);
    if (stream->png_ptr)
        png_destroy_read_struct(&stream->png_ptr, &stream->info_ptr, NULL);
    if (stream->input)
        fclose(stream->input);
    if (stream->pipefd[0] >= 0) close(stream->pipefd[0]);
    free(stream->row);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
    scanner->png_stream = NULL;
}

#else

SANE_Status
escl_png_stream_start(capabilities_t __sane_unused__ *scanner,
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
escl_png_stream_read(capabilities_t __sane_unused__ *scanner,
                     unsigned char __sane_unused__ *buf,
                     SANE_Int __sane_unused__ maxlen,
                     SANE_Int __sane_unused__ *len)
{
    return SANE_STATUS_UNSUPPORTED;
}

void
escl_png_stream_finish(capabilities_t __sane_unused__ *scanner)
{
}

#endif
