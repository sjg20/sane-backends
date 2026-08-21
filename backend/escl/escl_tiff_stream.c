/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 Thierry HUCHARD

   This file implements progressive TIFF streaming for the eSCL backend.  */

#define DEBUG_DECLARE_ONLY
#include "../include/sane/config.h"

#include "escl.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(HAVE_LIBTIFF) && defined(USE_PTHREAD)
#include <tiffio.h>

#define MAX_RETRIES 20

struct tiff_stream
{
    const ESCL_Device *device;
    char *scanJob;
    char *result;
    pthread_t thread;
    SANE_Bool thread_started;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    FILE *file;
    size_t size;
    toff_t offset;
    int write_fd;
    SANE_Bool done;
    SANE_Status status;
    TIFF *tif;
    unsigned char *raw;
    unsigned char *row;
    tmsize_t raw_size;
    int width;
    int height;
    int x_off;
    int y_off;
    int real_width;
    int real_height;
    int samples;
    int line;
    size_t row_size;
    size_t row_pos;
    SANE_Bool eof;
};

static size_t
tiff_stream_write(void *ptr, size_t size, size_t nmemb, void *data)
{
    struct tiff_stream *stream = data;
    size_t total = size * nmemb;
    pthread_mutex_lock(&stream->lock);
    if (fseek(stream->file, 0, SEEK_END) != 0) {
        pthread_mutex_unlock(&stream->lock);
        return 0;
    }
    size_t written = fwrite(ptr, 1, total, stream->file);
    if (written) {
        fflush(stream->file);
        stream->size += written;
        pthread_cond_broadcast(&stream->condition);
    }
    pthread_mutex_unlock(&stream->lock);
    return written;
}

static void *
tiff_stream_worker(void *data)
{
    struct tiff_stream *stream = data;
    char scan_cmd[PATH_MAX] = { 0 };
    CURL *curl_handle;

    snprintf(scan_cmd, sizeof(scan_cmd), "/eSCL/%s%s/NextDocument",
             stream->scanJob, stream->result);
    curl_handle = escl_curl_init(stream->device, scan_cmd);
    if (!curl_handle)
        stream->status = SANE_STATUS_NO_MEM;
    else {
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, tiff_stream_write);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, stream);
        stream->status = SANE_STATUS_IO_ERROR;
        for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
            CURLcode result = curl_easy_perform(curl_handle);
            SANE_Status status = escl_curl_status(curl_handle, result);
            if (escl_curl_retry(status, attempt, MAX_RETRIES))
                continue;
            stream->status = status;
            if (status == SANE_STATUS_GOOD && stream->size == 0)
                stream->status = SANE_STATUS_NO_DOCS;
            break;
        }
        curl_easy_cleanup(curl_handle);
    }
    pthread_mutex_lock(&stream->lock);
    stream->done = SANE_TRUE;
    pthread_cond_broadcast(&stream->condition);
    pthread_mutex_unlock(&stream->lock);
    return NULL;
}

static tmsize_t
tiff_stream_read(thandle_t handle, void *buffer, tmsize_t size)
{
    struct tiff_stream *stream = handle;
    toff_t offset;
    size_t wanted = (size_t)size;

    pthread_mutex_lock(&stream->lock);
    offset = stream->offset;
    while ((size_t)offset + wanted > stream->size && !stream->done)
        pthread_cond_wait(&stream->condition, &stream->lock);
    if ((size_t)offset + wanted > stream->size) {
        pthread_mutex_unlock(&stream->lock);
        return 0;
    }
    if (fseek(stream->file, (long)offset, SEEK_SET) != 0) {
        pthread_mutex_unlock(&stream->lock);
        return 0;
    }
    size_t read = fread(buffer, 1, wanted, stream->file);
    stream->offset += (toff_t)read;
    pthread_mutex_unlock(&stream->lock);
    return (tmsize_t)read;
}

static tmsize_t
tiff_stream_write_file(thandle_t __sane_unused__ handle,
                        void __sane_unused__ *buffer,
                        tmsize_t __sane_unused__ size)
{
    return 0;
}

static toff_t
tiff_stream_seek(thandle_t handle, toff_t offset, int whence)
{
    struct tiff_stream *stream = handle;
    toff_t position;
    pthread_mutex_lock(&stream->lock);
    if (whence == SEEK_END) {
        while (!stream->done)
            pthread_cond_wait(&stream->condition, &stream->lock);
        position = (toff_t)stream->size + offset;
    } else {
        position = offset;
    }
    if (fseek(stream->file, (long)position, SEEK_SET) != 0) {
        pthread_mutex_unlock(&stream->lock);
        return (toff_t)-1;
    }
    stream->offset = position;
    pthread_mutex_unlock(&stream->lock);
    return position;
}

static int
tiff_stream_close(thandle_t __sane_unused__ handle)
{
    return 0;
}

static toff_t
tiff_stream_size(thandle_t handle)
{
    struct tiff_stream *stream = handle;
    pthread_mutex_lock(&stream->lock);
    while (!stream->done)
        pthread_cond_wait(&stream->condition, &stream->lock);
    toff_t size = (toff_t)stream->size;
    pthread_mutex_unlock(&stream->lock);
    return size;
}

static int
tiff_stream_map(thandle_t __sane_unused__ handle,
                void **base,
                toff_t *size)
{
    (void)handle;
    (void)base;
    (void)size;
    return 0;
}

static void
tiff_stream_unmap(thandle_t __sane_unused__ handle,
                  void *base,
                  toff_t size)
{
    (void)handle;
    (void)base;
    (void)size;
}

static void
tiff_stream_dimensions(struct tiff_stream *stream, capabilities_t *scanner)
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
escl_tiff_stream_start(capabilities_t *scanner,
                       const ESCL_Device *device,
                       char *scanJob,
                       char *result,
                       int *width,
                       int *height,
                       int *bps)
{
    struct tiff_stream *stream = calloc(1, sizeof(*stream));
    uint16_t bits = 8, samples = 0, planar = PLANARCONFIG_CONTIG;
    uint16_t photometric = PHOTOMETRIC_RGB;
    if (!stream)
        return SANE_STATUS_NO_MEM;
    stream->device = device;
    stream->scanJob = scanJob;
    stream->result = result;
    stream->write_fd = -1;
    stream->file = tmpfile();
    pthread_mutex_init(&stream->lock, NULL);
    pthread_cond_init(&stream->condition, NULL);
    if (!stream->file || pthread_create(&stream->thread, NULL,
                                        tiff_stream_worker, stream) != 0)
        goto error;
    stream->thread_started = SANE_TRUE;
    stream->tif = TIFFClientOpen("escl-stream", "r", stream,
                                tiff_stream_read, tiff_stream_write_file,
                                tiff_stream_seek, tiff_stream_close,
                                tiff_stream_size, tiff_stream_map,
                                tiff_stream_unmap);
    if (!stream->tif)
        goto error;
    if (!TIFFGetField(stream->tif, TIFFTAG_IMAGEWIDTH, &stream->width) ||
        !TIFFGetField(stream->tif, TIFFTAG_IMAGELENGTH, &stream->height) ||
        !TIFFGetField(stream->tif, TIFFTAG_SAMPLESPERPIXEL, &samples) ||
        !TIFFGetField(stream->tif, TIFFTAG_BITSPERSAMPLE, &bits) ||
        !TIFFGetField(stream->tif, TIFFTAG_PLANARCONFIG, &planar))
        goto error;
    TIFFGetFieldDefaulted(stream->tif, TIFFTAG_PHOTOMETRIC, &photometric);
    if (bits != 8 || planar != PLANARCONFIG_CONTIG ||
        (samples != 1 && samples != 3) ||
        (photometric != PHOTOMETRIC_RGB && photometric != PHOTOMETRIC_MINISBLACK))
        goto unsupported;
    stream->raw_size = TIFFScanlineSize(stream->tif);
    if (stream->raw_size <= 0)
        goto error;
    tiff_stream_dimensions(stream, scanner);
    if (stream->real_width <= 0 || stream->real_height <= 0)
        goto unsupported;
    stream->samples = samples;
    stream->raw = malloc((size_t)stream->raw_size);
    stream->row_size = (size_t)stream->real_width * 3;
    stream->row = malloc(stream->row_size);
    if (!stream->raw || !stream->row)
        goto error;
    stream->row_pos = stream->row_size;
    scanner->tiff_stream = stream;
    *width = stream->real_width;
    *height = stream->real_height;
    *bps = 3;
    return SANE_STATUS_GOOD;

unsupported:
    /* Keep the downloaded file for the compatibility decoder. */
    if (stream->tif)
        TIFFClose(stream->tif);
    if (stream->thread_started)
        pthread_join(stream->thread, NULL);
    scanner->tmp = stream->file;
    stream->file = NULL;
    pthread_cond_destroy(&stream->condition);
    pthread_mutex_destroy(&stream->lock);
    free(stream->raw);
    free(stream->row);
    free(stream);
    return SANE_STATUS_GOOD;
error:
    escl_tiff_stream_finish((capabilities_t *)&(capabilities_t){ .tiff_stream = stream });
    return SANE_STATUS_INVAL;
}

SANE_Status
escl_tiff_stream_read(capabilities_t *scanner,
                      unsigned char *buf,
                      SANE_Int maxlen,
                      SANE_Int *len)
{
    struct tiff_stream *stream = scanner->tiff_stream;
    *len = 0;
    if (!stream || maxlen <= 0)
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
        if (stream->line >= stream->height) {
            stream->eof = SANE_TRUE;
            break;
        }
        if (TIFFReadScanline(stream->tif, stream->raw, stream->line, 0) < 0)
            return SANE_STATUS_INVAL;
        if (stream->line >= stream->y_off &&
            stream->line < stream->y_off + stream->real_height) {
            for (int x = 0; x < stream->real_width; x++) {
                size_t source = stream->samples == 1
                    ? (size_t)(stream->x_off + x)
                    : (size_t)(stream->x_off + x) * 3;
                if (stream->samples == 1) {
                    stream->row[x * 3] = stream->raw[source];
                    stream->row[x * 3 + 1] = stream->raw[source];
                    stream->row[x * 3 + 2] = stream->raw[source];
                } else if (source + 2 < (size_t)stream->raw_size) {
                    stream->row[x * 3] = stream->raw[source];
                    stream->row[x * 3 + 1] = stream->raw[source + 1];
                    stream->row[x * 3 + 2] = stream->raw[source + 2];
                } else {
                    stream->row[x * 3] = stream->row[x * 3 + 1] = stream->row[x * 3 + 2] = stream->raw[(size_t)(stream->x_off + x)];
                }
            }
            stream->row_pos = 0;
        }
        stream->line++;
    }
    if (*len == 0 && stream->eof)
        return SANE_STATUS_EOF;
    return SANE_STATUS_GOOD;
}

void
escl_tiff_stream_finish(capabilities_t *scanner)
{
    struct tiff_stream *stream;
    if (!scanner || !scanner->tiff_stream)
        return;
    stream = scanner->tiff_stream;
    if (stream->tif)
        TIFFClose(stream->tif);
    if (stream->thread_started)
        pthread_join(stream->thread, NULL);
    if (stream->file)
        fclose(stream->file);
    pthread_cond_destroy(&stream->condition);
    pthread_mutex_destroy(&stream->lock);
    free(stream->raw);
    free(stream->row);
    free(stream);
    scanner->tiff_stream = NULL;
}

#else

SANE_Status
escl_tiff_stream_start(capabilities_t __sane_unused__ *scanner,
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
escl_tiff_stream_read(capabilities_t __sane_unused__ *scanner,
                      unsigned char __sane_unused__ *buf,
                      SANE_Int __sane_unused__ maxlen,
                      SANE_Int __sane_unused__ *len)
{
    return SANE_STATUS_UNSUPPORTED;
}

void
escl_tiff_stream_finish(capabilities_t __sane_unused__ *scanner)
{
}

#endif
