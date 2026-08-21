/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 SANE Project

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
   If not, see <https://www.gnu.org/licenses/>. */

#define DEBUG_DECLARE_ONLY
#include "../include/sane/config.h"

#include "escl.h"

#include <string.h>

void
escl_hack_apply(ESCL_Device *device, SANE_String_Const server)
{
    if (!device)
        return;

    if ((device->model_name &&
         (strcasestr(device->model_name, "LaserJet FlowMFP M578") ||
          strcasestr(device->model_name, "LaserJet MFP M630"))) ||
        (server && strstr(server, "Server: HP_Compact_Server"))) {
        device->hacks.host_localhost = SANE_TRUE;
        if (!device->hack)
            device->hack = curl_slist_append(NULL, "Host: localhost");
    }

    if (device->model_name &&
        strcasestr(device->model_name, "MFC-J985DW"))
        device->hacks.disable_pdf = SANE_TRUE;
}
