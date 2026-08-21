/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 SANE Project

   This file is part of the SANE package and is distributed under the
   terms of the GNU General Public License version 3 or later.

   Shared HTTP helpers for the eSCL backend. */

#define DEBUG_DECLARE_ONLY
#include "../include/sane/config.h"

#include "escl.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define ESCL_MAX_REDIRECTS 3L
#define ESCL_RETRY_TIMEOUT 1

SANE_Bool
escl_parse_disable_https(SANE_String_Const line, SANE_Bool *disable_https)
{
    char directive[7] = { 0 };
    char name[14] = { 0 };
    char value[6] = { 0 };
    int fields;

    if (line == NULL || disable_https == NULL)
        return SANE_FALSE;

    fields = sscanf(line, "%6s %13s %5s", directive, name, value);
    if (fields < 2 ||
        strcmp(directive, "option") != 0 ||
        strcmp(name, "disable-https") != 0)
        return SANE_FALSE;

    *disable_https = (fields == 3 &&
                      (!strcasecmp(value, "yes") ||
                       !strcasecmp(value, "true") ||
                       !strcmp(value, "1")));
    return SANE_TRUE;
}

void
escl_curl_url(CURL *handle, const ESCL_Device *device, SANE_String_Const path)
{
    int url_len;
    char *url;

    if (!handle || !device || !device->ip_address || !path)
        return;

    url_len = snprintf(NULL, 0, "%s://%s:%d%s",
                       (device->https ? "https" : "http"), device->ip_address,
                       device->port_nb, path) + 1;
    url = (char *)malloc(url_len);
    if (!url)
        return;
    snprintf(url, url_len, "%s://%s:%d%s",
             (device->https ? "https" : "http"), device->ip_address,
             device->port_nb, path);

    DBG(10, "escl_curl_url: URL: %s\n", url);
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, ESCL_CONNECT_TIMEOUT);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, ESCL_REQUEST_TIMEOUT);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, ESCL_MAX_REDIRECTS);
    free(url);
    if (device->hack)
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, device->hack);
    if (device->https) {
        DBG(10, "Ignoring safety certificates, use https\n");
        curl_easy_setopt(handle, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (device->unix_socket != NULL)
        curl_easy_setopt(handle, CURLOPT_UNIX_SOCKET_PATH, device->unix_socket);
}

CURL *
escl_curl_init(const ESCL_Device *device, SANE_String_Const path)
{
    CURL *handle = curl_easy_init();

    if (handle)
        escl_curl_url(handle, device, path);
    return handle;
}

SANE_Status
escl_http_status(long response)
{
    if (response >= 200 && response < 300)
        return SANE_STATUS_GOOD;

    DBG(10, "eSCL HTTP status: %ld\n", response);
    switch (response) {
    case 401:
    case 403:
        return SANE_STATUS_ACCESS_DENIED;
    case 404:
    case 409:
        return SANE_STATUS_NO_DOCS;
    case 423:
    case 429:
    case 503:
        return SANE_STATUS_DEVICE_BUSY;
    case 400:
    case 405:
    case 415:
        return SANE_STATUS_UNSUPPORTED;
    default:
        return SANE_STATUS_IO_ERROR;
    }
}

SANE_Status
escl_curl_status(CURL *handle, CURLcode result)
{
    long response = 0;

    if (result != CURLE_OK) {
        DBG(10, "eSCL transport error: %s\n", curl_easy_strerror(result));
        return SANE_STATUS_IO_ERROR;
    }
    if (curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response) != CURLE_OK)
        return SANE_STATUS_IO_ERROR;
    return escl_http_status(response);
}

SANE_Bool
escl_curl_retry(SANE_Status status, int attempt, int max_attempts)
{
    if (status != SANE_STATUS_DEVICE_BUSY ||
        attempt + 1 >= max_attempts)
        return SANE_FALSE;

    sleep(ESCL_RETRY_TIMEOUT);
    return SANE_TRUE;
}
