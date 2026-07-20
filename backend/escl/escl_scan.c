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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_RETRIES 20
#define RETRY_TIMEOUT 1

/**
 * \fn static size_t write_callback(void *str, size_t size, size_t nmemb, void *userp)
 * \brief Callback function that writes the image scanned into the temporary file.
 *
 * \return to_write (the result of the fwrite function)
 */
static size_t
write_callback(void *str, size_t size, size_t nmemb, void *userp)
{
    capabilities_t *scanner = (capabilities_t *)userp;
    size_t to_write = fwrite(str, size, nmemb, scanner->tmp);
    scanner->real_read += to_write;
    return (to_write);
}

/**
 * \fn SANE_Status escl_scan(capabilities_t *scanner, const ESCL_Device *device, char *result)
 * \brief Function that, after recovering the 'new job', scans the image writed in the
 *        temporary file, using curl.
 *        This function is called in the 'sane_start' function and it's the equivalent of
 *        the following curl command : "curl -s http(s)://'ip:'port'/eSCL/ScanJobs/'new job'/NextDocument > image.jpg".
 *
 * \return status (if everything is OK, status = SANE_STATUS_GOOD, otherwise, SANE_STATUS_NO_MEM/SANE_STATUS_INVAL)
 */
SANE_Status
escl_scan(capabilities_t *scanner, const ESCL_Device *device, char *scanJob, char *result)
{
    CURL *curl_handle = NULL;
    const char *scan_jobs = "/eSCL/";
    const char *scanner_start = "/NextDocument";
    char scan_cmd[PATH_MAX] = { 0 };
    SANE_Status status = SANE_STATUS_GOOD;
    long response_code = 0;

    if (device == NULL)
        return SANE_STATUS_NO_MEM;

    if (scanner->tmp)
        fclose(scanner->tmp);
    scanner->tmp = tmpfile();
    if (!scanner->tmp)
        return SANE_STATUS_NO_MEM;

    curl_handle = curl_easy_init();
    if (!curl_handle) {
        status = SANE_STATUS_NO_MEM;
        goto cleanup;
    }

    snprintf(scan_cmd, sizeof(scan_cmd), "%s%s%s%s",
             scan_jobs, scanJob, result, scanner_start);
    escl_curl_url(curl_handle, device, scan_cmd);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, scanner);

    for (int i = 0; i < MAX_RETRIES && response_code != 200; i++) {
        scanner->real_read = 0;
        CURLcode res = curl_easy_perform(curl_handle);
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
        if (res != CURLE_OK || (response_code != 200 && response_code != 503)) {
            DBG( 10, "Unable to scan: %s (response code %ld)\n", curl_easy_strerror(res), response_code);
            status = SANE_STATUS_INVAL;
            goto cleanup;
        } else if (response_code == 503) {
            status = SANE_STATUS_DEVICE_BUSY;
            sleep(RETRY_TIMEOUT);
            DBG(10, "Service unavailable: reattempting scan (%d/%d)\n", i + 1, MAX_RETRIES);
        } else {
            status = scanner->real_read > 0 ? SANE_STATUS_GOOD : SANE_STATUS_NO_DOCS;
        }

        if (scanner->real_read)
            fseek(scanner->tmp, 0, SEEK_SET);
    }
cleanup:
    curl_easy_cleanup(curl_handle);
    DBG(10, "eSCL scan : [%s]\treal read (%ld)\n", sane_strstatus(status), scanner->real_read);
    if (status != SANE_STATUS_GOOD && scanner->tmp) {
        fclose(scanner->tmp);
        scanner->tmp = NULL;
    }
    return status;
}
