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

#include <stdlib.h>
#include <string.h>

static size_t
write_callback(void __sane_unused__*str,
               size_t __sane_unused__ size,
               size_t nmemb,
               void __sane_unused__ *userp)
{
    return nmemb;
}

/**
 * \fn void escl_scanner(const ESCL_Device *device, char *result)
 * \brief Function that resets the scanner after each scan, using curl.
 *        This function is called in the 'sane_cancel' function.
 */
void
escl_delete(const ESCL_Device *device, char *uri)
{
    CURL *curl_handle = NULL;

    if (uri == NULL)
        return;
    if (!device || !uri)
        return;
    curl_handle = escl_curl_init(device, uri);
    if (curl_handle != NULL) {
	 curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
        CURLcode result = curl_easy_perform(curl_handle);
        (void)escl_curl_status(curl_handle, result);
        curl_easy_cleanup(curl_handle);
    }
}

/**
 * \fn void escl_scanner(const ESCL_Device *device, char *result)
 * \brief Function that resets the scanner after each scan, using curl.
 *        This function is called in the 'sane_cancel' function.
 */
void
escl_scanner(const ESCL_Device *device, char *scanJob, char *result,  SANE_Bool status)
{
    CURL *curl_handle = NULL;
    const char *scan_jobs = "/eSCL/";
    const char *scanner_start = "/NextDocument";
    char scan_cmd[PATH_MAX] = { 0 };
    int i = 0;

    if (device == NULL || result == NULL)
        return;
CURL_CALL:
    snprintf(scan_cmd, sizeof(scan_cmd), "%s%s%s%s",
             scan_jobs, scanJob, result, scanner_start);
    curl_handle = escl_curl_init(device, scan_cmd);
    if (!curl_handle)
        return;
    if (curl_handle != NULL) {
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
        CURLcode result = curl_easy_perform(curl_handle);
        if (escl_curl_status(curl_handle, result) == SANE_STATUS_GOOD) {
            i++;
        }
        curl_easy_cleanup(curl_handle);
	if (i >= 15) return;
	char* end = strrchr(scan_cmd, '/');
	*end = 0;
        escl_delete(device, scan_cmd);
	if (status) {
            if (SANE_STATUS_GOOD != escl_status(device,
                                                PLATEN,
                                                NULL,
                                                NULL))
                goto CURL_CALL;
	}
    }
}
