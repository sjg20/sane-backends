/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 Simon Glass <sjg@chromium.org>

   This file is part of the SANE package.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.

   As a special exception, the authors of SANE give permission for
   additional uses of the libraries contained in this release of SANE.

   The exception is that, if you link a SANE library with other files
   to produce an executable, this does not by itself cause the
   resulting executable to be covered by the GNU General Public
   License.  Your use of that executable is in no way restricted on
   account of linking the SANE library code into it.

   This exception does not, however, invalidate any other reasons why
   the executable file might be covered by the GNU General Public
   License.

   If you submit changes to SANE to the maintainers to be included in
   a subsequent release, you agree by submitting the changes that
   those changes may be distributed with this exception intact.

   If you write modifications of your own for SANE, it is your choice
   whether to permit this exception to apply to your modifications.
   If you do not wish that, delete this exception notice.

   This backend drives the network interface of the Ricoh (formerly
   Fujitsu/PFU) fi-8000 series document scanners, such as the fi-8950.

   The scanner offers a small HTTP interface on port 80 modelled on the
   Privet protocol: GET /api/privet/info describes the device and
   POST /api/privet/session carries JSON commands (createSession,
   getSession, sendTask, startCapturing, readImageBlock,
   releaseImageBlocks, stopCapturing, closeSession). A scan task
   describes the paper feed and pixel format; each scanned side is then
   fetched as a JPEG through the URI returned by readImageBlock. The
   scanner always returns colour JPEG at the requested resolution, so
   greyscale and lineart are produced here when decoding.

   The protocol was worked out by watching the vendor's PaperStream IP
   driver talk to an fi-8950; there is no public documentation. */

#define DEBUG_NOT_STATIC
#define BUILD 1

#include "../include/sane/config.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <pthread.h>
#include <jpeglib.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "../include/sane/sane.h"
#include "../include/sane/saneopts.h"
#include "../include/sane/sanei.h"
#include "../include/sane/sanei_config.h"

#define BACKEND_NAME finet
#include "../include/sane/sanei_backend.h"

#define FINET_CONFIG_FILE "finet.conf"
#define FINET_TOKEN "***CLIENTACCESSTOKEN***"
#define FINET_DEFAULT_PORT 80

/* the scanners answer a broadcast probe on this UDP port with a packet
   that starts with this magic, giving their address and model */
#define FINET_DISCOVERY_PORT 52217
#define FINET_MAGIC "fiCH"
#define FINET_DISCOVERY_MS 1500

/* the scanner measures paper in 1/1200 inch */
#define UNITS_PER_INCH 1200
#define MM_PER_INCH 25.4

/* the fi-8950 bed: 12 by 17 inches */
#define MAX_WIDTH_MM 304.8
#define MAX_HEIGHT_MM 431.8

#define MAX_JSON (256 * 1024)
#define MAX_IMAGE (64 * 1024 * 1024)

/* how long to let the scanner take over a single command */
#define CONNECT_TIMEOUT 10L
#define COMMAND_TIMEOUT 120L

/* how long to wait for the next image before giving up on the scanner;
   it answers 'noImage' about every ten seconds while nothing arrives */
#define IMAGE_TIMEOUT 120

enum finet_option
{
  OPT_NUM_OPTS = 0,

  OPT_STANDARD_GROUP,
  OPT_SOURCE,
  OPT_MODE,
  OPT_RESOLUTION,

  OPT_GEOMETRY_GROUP,
  OPT_AUTO_SIZE,
  OPT_TL_X,
  OPT_TL_Y,
  OPT_BR_X,
  OPT_BR_Y,
  OPT_PAGE_WIDTH,
  OPT_PAGE_HEIGHT,

  OPT_ENHANCEMENT_GROUP,
  OPT_BRIGHTNESS,
  OPT_CONTRAST,
  OPT_THRESHOLD,
  OPT_DESKEW,

  OPT_ADVANCED_GROUP,
  OPT_COMPRESSION,
  OPT_JPEG_QUALITY,
  OPT_BG_COLOUR,
  OPT_DF_DETECT,
  OPT_PREPICK,
  OPT_STOP_FEED,
  OPT_IMAGES_WAITING,

  NUM_OPTIONS
};

/* The scanner always scans both sides of the sheet; the source decides
   which of the two images are fetched */
static const SANE_String_Const source_list[] = {
  "ADF Front", "ADF Back", "ADF Duplex", NULL
};
#define SOURCE_FRONT 0
#define SOURCE_BACK 1
#define SOURCE_DUPLEX 2

static const SANE_String_Const mode_list[] = {
  SANE_VALUE_SCAN_MODE_LINEART, SANE_VALUE_SCAN_MODE_GRAY,
  SANE_VALUE_SCAN_MODE_COLOR, NULL
};

static const SANE_String_Const compression_list[] = {
  "None", "JPEG", NULL
};

static const SANE_String_Const bg_colour_list[] = {
  "White", "Black", NULL
};

static const SANE_Range resolution_range = { 50, 600, 1 };

struct finet_scanner;
static SANE_Bool want_passthrough (struct finet_scanner *s);
static const SANE_Range threshold_range = { 0, 255, 1 };
static const SANE_Range level_range = { -127, 127, 1 };
static const SANE_Range quality_range = { 1, 100, 1 };
static SANE_Range x_range;
static SANE_Range y_range;

/* a scanner named in the configuration file */
struct finet_device
{
  struct finet_device *next;
  char *host;
  int port;
  char *name;                   /* <host> or <host>:<port> */
  char *model;
  char *serial;
  SANE_Device sane;
};

/* a growable byte buffer for HTTP responses */
struct buffer
{
  unsigned char *data;
  size_t size;
  size_t max;
};

/* an open scanner */
struct finet_scanner
{
  struct finet_scanner *next;
  struct finet_device *dev;
  CURL *curl;

  SANE_Option_Descriptor opt[NUM_OPTIONS];
  Option_Value val[NUM_OPTIONS];

  char session[64];             /* current session id, or empty */
  SANE_Bool capturing;          /* a capture session is open */
  SANE_Bool feed_stopped;       /* the feeder is paused; drain the rest */
  int pending_upto;             /* blocks up to this were seen waiting */
  time_t queue_polled;          /* when the waiting count was last asked */
  int block;                    /* next image block number */
  int to_release;               /* a fetched block not yet released, or 0 */
  struct
  {
    pthread_t thread;
    SANE_Bool running;          /* the thread is fetching */
    SANE_Bool abort;            /* give up the fetch: set by sane_cancel() */
    SANE_Bool valid;            /* the result below is ready and unused */
    int block;                  /* which block it fetched */
    SANE_Bool want;             /* whether it fetched the JPEG too */
    SANE_Status status;
    struct buffer meta;         /* the readImageBlock reply */
    struct buffer jpeg;
  } pf;                         /* a block fetched in the background */
  int last_w, last_h;           /* last scanned size, for the auto estimate */
  SANE_Bool cancelled;

  SANE_Parameters params;
  unsigned char *image;         /* the decoded, converted image */
  size_t image_size;
  size_t image_pos;
  SANE_Bool eof_pending;        /* the image has been handed out */
  SANE_Bool passthrough;        /* the image is the scanner's JPEG as is */
};

static SANE_Bool discovery_disabled;
static struct finet_device *device_list;
static struct finet_scanner *scanner_list;
static const SANE_Device **sane_device_list;

/* ---------------------------------------------------------------------- */
/* HTTP */

static size_t
write_cb (void *ptr, size_t size, size_t nmemb, void *data)
{
  struct buffer *buf = data;
  size_t count = size * nmemb;

  if (buf->size + count + 1 > buf->max)
    {
      size_t max = buf->max ? buf->max * 2 : 65536;
      unsigned char *p;

      while (max < buf->size + count + 1)
        max *= 2;
      if (max > MAX_IMAGE)
        return 0;
      p = realloc (buf->data, max);
      if (!p)
        return 0;
      buf->data = p;
      buf->max = max;
    }
  memcpy (buf->data + buf->size, ptr, count);
  buf->size += count;
  buf->data[buf->size] = 0;
  return count;
}

static void
buffer_free (struct buffer *buf)
{
  free (buf->data);
  buf->data = NULL;
  buf->size = buf->max = 0;
}

/* Perform a request; body is NULL for GET. The response lands in buf */
/* curl asks this regularly during a transfer: a non-zero return aborts.
   Only a fetch is ever abandoned: the commands that then close the
   session must still go through, so the flag is cleared before them */
static int
progress_cb (void *arg, curl_off_t dltotal, curl_off_t dlnow,
             curl_off_t ultotal, curl_off_t ulnow)
{
  struct finet_scanner *s = arg;

  (void) dltotal; (void) dlnow; (void) ultotal; (void) ulnow;
  return s->pf.abort;
}

static SANE_Status
http (struct finet_scanner *s, const char *path, const char *body,
      long timeout, struct buffer *buf)
{
  struct curl_slist *headers = NULL;
  char url[512];
  CURLcode res;
  long code = 0;

  buffer_free (buf);
  snprintf (url, sizeof (url), "http://%s:%d%s", s->dev->host, s->dev->port,
            path);
  DBG (15, "%s %s%s\n", body ? "POST" : "GET", url, body ? " " : "");
  if (body)
    DBG (20, "  %s\n", body);

  curl_easy_reset (s->curl);
  curl_easy_setopt (s->curl, CURLOPT_URL, url);
  /* let a cancel cut short a readImageBlock the scanner is holding */
  curl_easy_setopt (s->curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt (s->curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
  curl_easy_setopt (s->curl, CURLOPT_XFERINFODATA, s);
  curl_easy_setopt (s->curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);
  curl_easy_setopt (s->curl, CURLOPT_TIMEOUT, timeout);
  curl_easy_setopt (s->curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt (s->curl, CURLOPT_WRITEDATA, buf);
  headers = curl_slist_append (headers, "Authorization: Bearer " FINET_TOKEN);
  if (body)
    {
      headers = curl_slist_append (headers, "Content-Type: application/json");
      curl_easy_setopt (s->curl, CURLOPT_POSTFIELDS, body);
    }
  curl_easy_setopt (s->curl, CURLOPT_HTTPHEADER, headers);
  res = curl_easy_perform (s->curl);
  curl_easy_getinfo (s->curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all (headers);

  if (res != CURLE_OK)
    {
      DBG (1, "%s: %s\n", url, curl_easy_strerror (res));
      return SANE_STATUS_IO_ERROR;
    }
  if (code < 200 || code >= 300)
    {
      DBG (1, "%s: HTTP %ld\n", url, code);
      return SANE_STATUS_IO_ERROR;
    }
  DBG (20, "  -> %zu bytes\n", buf->size);
  return SANE_STATUS_GOOD;
}

/* ---------------------------------------------------------------------- */
/* JSON: the replies are small and their key names are unambiguous, so a
   search for "key": is enough to pick out the fields used here */

/* find the value following "key": and copy it (unquoted) into out */
static SANE_Bool
json_get (const char *json, const char *key, char *out, size_t size)
{
  char pat[64];
  const char *p;

  snprintf (pat, sizeof (pat), "\"%s\":", key);
  p = strstr (json, pat);
  if (!p)
    return SANE_FALSE;
  p += strlen (pat);
  while (*p == ' ')
    p++;
  if (*p == '"')
    {
      size_t n = 0;

      /* copy the quoted string, undoing the JSON escapes the scanner
         uses (notably \/ in the image URIs) */
      p++;
      while (*p && *p != '"' && n < size - 1)
        {
          if (*p == '\\' && p[1])
            {
              p++;
              switch (*p)
                {
                case 'n': out[n++] = '\n'; break;
                case 't': out[n++] = '\t'; break;
                case 'r': out[n++] = '\r'; break;
                default: out[n++] = *p; break;
                }
              p++;
            }
          else
            out[n++] = *p++;
        }
      out[n] = 0;
      return SANE_TRUE;
    }
  else
    {
      size_t n = 0;

      while (*p && n < size - 1 && (isalnum ((unsigned char) *p) || *p == '-'
                                    || *p == '.'))
        out[n++] = *p++;
      out[n] = 0;
      return n > 0;
    }
}

static int
json_get_int (const char *json, const char *key, int def)
{
  char tmp[32];

  if (!json_get (json, key, tmp, sizeof (tmp)))
    return def;
  return atoi (tmp);
}

/* did the scanner accept the command? */
static SANE_Bool
json_ok (const char *json)
{
  return strstr (json, "\"status\":\"success\"") != NULL;
}

/* ---------------------------------------------------------------------- */
/* The scanner's commands */

static SANE_Status
command (struct finet_scanner *s, const char *method, const char *params,
         int timeout, struct buffer *buf)
{
  char body[4096 + 2048];
  SANE_Status status;

  snprintf (body, sizeof (body),
            "{\"commandId\":\"sane\",\"commandTimeOut\":\"%d\","
            "\"method\":\"%s\",\"parameters\":{%s}}", timeout, method, params);
  status = http (s, "/api/privet/session", body, COMMAND_TIMEOUT, buf);
  if (status != SANE_STATUS_GOOD)
    return status;
  /* readImageBlock answers 'noImage' rather than 'success' when nothing
     arrives within its timeout: that is an answer, not a failure, and the
     caller looks for it */
  if (!json_ok ((const char *) buf->data)
      && !strstr ((const char *) buf->data, "\"status\":\"noImage\""))
    {
      DBG (1, "%s failed: %.300s\n", method, (const char *) buf->data);
      return SANE_STATUS_IO_ERROR;
    }
  return SANE_STATUS_GOOD;
}

static SANE_Status
get_info (struct finet_scanner *s, struct buffer *buf)
{
  return http (s, "/api/privet/info", NULL, CONNECT_TIMEOUT * 2, buf);
}

static SANE_Status
open_session (struct finet_scanner *s)
{
  struct buffer buf = { 0 };
  SANE_Status status;

  if (s->session[0])
    return SANE_STATUS_GOOD;
  status = command (s, "createSession", "\"connect\":\"PSIP\"", 5, &buf);
  if (status == SANE_STATUS_GOOD
      && !json_get ((const char *) buf.data, "sessionId", s->session,
                    sizeof (s->session)))
    status = SANE_STATUS_IO_ERROR;
  buffer_free (&buf);
  DBG (10, "session %s\n", s->session);
  return status;
}

static void prefetch_drop (struct finet_scanner *s);
static void prefetch_wait (struct finet_scanner *s);

static void
close_session (struct finet_scanner *s)
{
  struct buffer buf = { 0 };
  char params[128];

  if (!s->session[0])
    return;
  prefetch_drop (s);
  if (s->capturing)
    {
      if (s->to_release)
        {
          snprintf (params, sizeof (params),
                    "\"imageBlockNum\":%d,\"sessionId\":\"%s\"",
                    s->to_release, s->session);
          command (s, "releaseImageBlocks", params, 60, &buf);
          s->to_release = 0;
        }
      snprintf (params, sizeof (params),
                "\"pauseScanning\":\"false\",\"sessionId\":\"%s\"", s->session);
      command (s, "stopCapturing", params, 5, &buf);
      s->capturing = SANE_FALSE;
      s->feed_stopped = SANE_FALSE;
      s->pending_upto = 0;
      s->val[OPT_IMAGES_WAITING].w = 0;
    }
  snprintf (params, sizeof (params), "\"sessionId\":\"%s\"", s->session);
  command (s, "closeSession", params, 5, &buf);
  buffer_free (&buf);
  s->session[0] = 0;
}

/* is there paper in the hopper? */
static SANE_Status
check_hopper (struct finet_scanner *s)
{
  struct buffer buf = { 0 };
  char params[128];
  char val[32];
  SANE_Status status;

  snprintf (params, sizeof (params),
            "\"operationCode\":1,\"sessionId\":\"%s\"", s->session);
  status = command (s, "getSession", params, 5, &buf);
  if (status == SANE_STATUS_GOOD)
    {
      const char *json = (const char *) buf.data;

      if (json_get (json, "hopperEmpty", val, sizeof (val))
          && !strcmp (val, "on"))
        status = SANE_STATUS_NO_DOCS;
      else if (json_get (json, "adfCover", val, sizeof (val))
               && !strcmp (val, "on"))
        status = SANE_STATUS_COVER_OPEN;
    }
  buffer_free (&buf);
  return status;
}

/* Stop the feeder but keep the images already captured. With
   pauseScanning the scanner holds them until they are read, so a batch
   that is stopped early still yields every sheet that went through;
   without it they are discarded. The batch ends once they are drained */
static SANE_Status
stop_feed (struct finet_scanner *s)
{
  struct buffer buf = { 0 };
  char params[128];
  SANE_Status status;

  if (!s->capturing || s->feed_stopped)
    return SANE_STATUS_GOOD;
  prefetch_wait (s);
  snprintf (params, sizeof (params),
            "\"pauseScanning\":\"true\",\"sessionId\":\"%s\"", s->session);
  status = command (s, "stopCapturing", params, 5, &buf);
  buffer_free (&buf);
  if (status == SANE_STATUS_GOOD)
    s->feed_stopped = SANE_TRUE;
  DBG (10, "feeder stopped: %d\n", status);
  return status;
}

/* How many blocks does the session list beyond this one? The list holds
   every captured image not yet released, so that is the queue of images
   the scanner has ready and we have not fetched */
static int
count_waiting (const char *json, int block)
{
  const char *p = strstr (json, "\"imageBlocks\":[");
  int n = 0;

  if (!p)
    return 0;
  for (p += 15; *p && *p != ']'; p++)
    if (*p != ',' && atoi (p) > block)
      {
        n++;
        while (p[1] && p[1] != ',' && p[1] != ']')
          p++;
      }
  return n;
}

/* Keep the images-waiting option up to date, asking at most once a
   second so the round trip does not slow the fetching */
static void
poll_queue (struct finet_scanner *s, int block)
{
  struct buffer buf = { 0 };
  char params[128];
  time_t now = time (NULL);

  if (now == s->queue_polled)
    return;
  s->queue_polled = now;
  snprintf (params, sizeof (params),
            "\"operationCode\":1,\"sessionId\":\"%s\"", s->session);
  if (command (s, "getSession", params, 5, &buf) == SANE_STATUS_GOOD)
    s->val[OPT_IMAGES_WAITING].w = count_waiting ((const char *) buf.data,
                                                  block);
  buffer_free (&buf);
}

/* Once the feeder is stopped, is anything still to come? The session
   lists the captured blocks not yet released, and a sheet still in the
   paper path shows on the pick and top sensors until it is scanned and
   listed. Asking is quick, whereas readImageBlock holds for ten seconds
   when nothing is there, whatever timeout it is given */
static SANE_Bool
image_pending (struct finet_scanner *s)
{
  struct buffer buf = { 0 };
  char params[128];
  char pick[8], top[8];
  int i;

  snprintf (params, sizeof (params),
            "\"operationCode\":1,\"sessionId\":\"%s\"", s->session);
  for (i = 0; i < 20; i++)
    {
      const char *json;
      SANE_Bool listed, in_path;

      if (i)
        usleep (500000);
      if (command (s, "getSession", params, 5, &buf) != SANE_STATUS_GOOD)
        break;
      json = (const char *) buf.data;
      listed = strstr (json, "\"imageBlocks\":[]") == NULL
        && strstr (json, "\"imageBlocks\":[") != NULL;
      /* remember the last block listed, so the blocks before it need no
         asking: each ask is a round trip */
      if (listed)
        {
          const char *p = strstr (json, "\"imageBlocks\":[") + 15;
          const char *q;

          for (q = p; *q && *q != ']'; q++)
            if (*q == ',')
              p = q + 1;
          s->pending_upto = atoi (p);
          s->val[OPT_IMAGES_WAITING].w = count_waiting (json, s->block - 1);
        }
      in_path = (json_get (json, "pick", pick, sizeof (pick))
                 && !strcmp (pick, "on"))
        || (json_get (json, "top", top, sizeof (top)) && !strcmp (top, "on"));
      buffer_free (&buf);
      if (listed)
        return SANE_TRUE;
      /* a couple of seconds' grace for a sheet the sensors have not
         reported yet, then trust them */
      if (!in_path && i >= 4)
        return SANE_FALSE;
    }
  buffer_free (&buf);
  return SANE_FALSE;
}

static int
mm_to_units (SANE_Fixed mm)
{
  return (int) (SANE_UNFIX (mm) / MM_PER_INCH * UNITS_PER_INCH + 0.5);
}

/* one attribute of a task, in the scanner's verbose JSON form */
static void
attr (char *out, size_t size, const char *name, const char *value,
      SANE_Bool last)
{
  size_t len = strlen (out);

  snprintf (out + len, size - len,
            "{\"attribute\":\"%s\",\"values\":{\"value\":\"%s\"}}%s", name,
            value, last ? "" : ",");
}

static void
attr_int (char *out, size_t size, const char *name, int value, SANE_Bool last)
{
  char tmp[32];

  snprintf (tmp, sizeof (tmp), "%d", value);
  attr (out, size, name, tmp, last);
}

/* Turn the fixed scan-area options on or off, following auto-size */
static void
set_geometry_active (struct finet_scanner *s, SANE_Bool active)
{
  int opt[] = { OPT_TL_X, OPT_TL_Y, OPT_BR_X, OPT_BR_Y,
    OPT_PAGE_WIDTH, OPT_PAGE_HEIGHT };
  size_t i;

  for (i = 0; i < sizeof (opt) / sizeof (opt[0]); i++)
    {
      if (active)
        s->opt[opt[i]].cap &= ~SANE_CAP_INACTIVE;
      else
        s->opt[opt[i]].cap |= SANE_CAP_INACTIVE;
    }
}

/* Which entry of source_list is selected */
static int
source_index (struct finet_scanner *s)
{
  int i;

  for (i = 0; source_list[i]; i++)
    if (!strcmp (s->val[OPT_SOURCE].s, source_list[i]))
      return i;
  return 0;
}

/* Describe the scan to the scanner: every sheet in the hopper (a count
   of 0), at the chosen resolution and frame, as JPEG */
static SANE_Status
send_task (struct finet_scanner *s)
{
  struct buffer buf = { 0 };
  char pixel[2048] = "";
  char params[4096];
  SANE_Bool autosize = s->val[OPT_AUTO_SIZE].w;
  /* auto-size crops to the paper the scanner finds, up to the window, so
     the window is opened to the full bed */
  int width = autosize ? mm_to_units (SANE_FIX (MAX_WIDTH_MM))
    : mm_to_units (s->val[OPT_BR_X].w - s->val[OPT_TL_X].w);
  int height = autosize ? mm_to_units (SANE_FIX (MAX_HEIGHT_MM))
    : mm_to_units (s->val[OPT_BR_Y].w - s->val[OPT_TL_Y].w);
  SANE_Status status;

  attr_int (pixel, sizeof (pixel), "resolution", s->val[OPT_RESOLUTION].w, 0);
  attr_int (pixel, sizeof (pixel), "width", width, 0);
  attr_int (pixel, sizeof (pixel), "height", height, 0);
  attr_int (pixel, sizeof (pixel), "offsetWidth",
            autosize ? 0 : mm_to_units (s->val[OPT_TL_X].w), 0);
  attr_int (pixel, sizeof (pixel), "offsetHeight",
            autosize ? 0 : mm_to_units (s->val[OPT_TL_Y].w), 0);
  /* the scanner's own automaticSize pads with white and crops
     inconsistently; for auto-size scan the full window on the black
     backing (set above) and crop to the page in the backend instead */
  attr (pixel, sizeof (pixel), "automaticSize", "disable", 0);
  /* the scanner only ever produces colour JPEG: grey and lineart are
     made from it here */
  attr (pixel, sizeof (pixel), "compression", "jpeg", 0);
  attr_int (pixel, sizeof (pixel), "jpegQuality", s->val[OPT_JPEG_QUALITY].w,
            0);
  attr (pixel, sizeof (pixel), "jpgSubSampling", "422", 0);
  attr (pixel, sizeof (pixel), "overscan", "off", 0);
  attr (pixel, sizeof (pixel), "automaticDeskew",
        s->val[OPT_DESKEW].w ? "enable" : "disable", 0);
  attr (pixel, sizeof (pixel), "endOfPageDetection", "off", 0);
  attr_int (pixel, sizeof (pixel), "paperWidth",
            autosize ? width : mm_to_units (s->val[OPT_PAGE_WIDTH].w), 0);
  attr_int (pixel, sizeof (pixel), "paperLength",
            autosize ? height : mm_to_units (s->val[OPT_PAGE_HEIGHT].w), 1);

  snprintf (params, sizeof (params),
            "\"sessionId\":\"%s\",\"task\":{\"actions\":{\"streams\":{"
            "\"sources\":{"
            "\"feedControls\":{"
            "\"numberOfSheets\":{\"attributes\":["
            "{\"attribute\":\"sheetCounts\",\"values\":{\"value\":\"0\"}}]},"
            "\"doubleFeed\":{\"attributes\":["
            "{\"attribute\":\"overlap\",\"values\":{\"value\":\"%s\"}},"
            "{\"attribute\":\"length\",\"values\":{\"value\":\"disable\"}},"
            "{\"attribute\":\"response\",\"values\":{\"value\":\"recovery\"}},"
            "{\"attribute\":\"deviceSpecification\",\"values\":{\"value\":\"disable\"}},"
            "{\"attribute\":\"iOMFLength\",\"values\":{\"value\":\"0\"}}]},"
            "\"background\":{\"attributes\":["
            "{\"attribute\":\"bgColor\",\"values\":{\"value\":\"%s\"}}]},"
            "\"prePick\":{\"attributes\":["
            "{\"attribute\":\"prePickControl\",\"values\":{\"value\":\"%s\"}}]}"
            "},"
            "\"pixelFormats\":{\"attributes\":[%s]},"
            "\"readControls\":{"
            /* divided delivery: the image may be fetched in parts, and the
               scanner hands images over about a quarter faster this way
               even when each comes as a single part */
            "\"devidedSize\":{\"attributes\":["
            "{\"attribute\":\"devidedSize\",\"values\":{\"value\":\"12\"}}]},"
            "\"imageCacheMode\":{\"attributes\":["
            "{\"attribute\":\"imageCacheMode\",\"values\":{\"value\":\"scannerMemory\"}}]},"
            "\"imageTransferMethod\":{\"attributes\":["
            "{\"attribute\":\"imageTransferMethod\",\"values\":{\"value\":\"alternate\"}}]}"
            "}}}}}", s->session,
            s->val[OPT_DF_DETECT].w ? "enable" : "disable",
            (autosize || !strcmp (s->val[OPT_BG_COLOUR].s, bg_colour_list[1]))
            ? "black" : "white",
            s->val[OPT_PREPICK].w ? "enable" : "disable", pixel);
  status = command (s, "sendTask", params, 5, &buf);
  buffer_free (&buf);
  return status;
}

static SANE_Status
start_capturing (struct finet_scanner *s)
{
  struct buffer buf = { 0 };
  char params[192];
  char val[32];
  SANE_Status status;

  snprintf (params, sizeof (params),
            "\"ignore_mf_detection\":\"false\",\"restartCapturing\":\"false\","
            "\"sessionId\":\"%s\"", s->session);
  status = command (s, "startCapturing", params, 35, &buf);
  if (status == SANE_STATUS_GOOD)
    {
      json_get ((const char *) buf.data, "detected", val, sizeof (val));
      DBG (10, "startCapturing: %s\n", val);
      if (strcmp (val, "success"))
        status = SANE_STATUS_NO_DOCS;
      else
        s->capturing = SANE_TRUE;
    }
  buffer_free (&buf);
  return status;
}

/* ---------------------------------------------------------------------- */
/* Images */

struct jpeg_error
{
  struct jpeg_error_mgr mgr;
  jmp_buf jump;
};

static void
jpeg_fail (j_common_ptr cinfo)
{
  struct jpeg_error *err = (struct jpeg_error *) cinfo->err;

  longjmp (err->jump, 1);
}

/* The scanner's JPEG is always colour, so it can only be handed over as
   it is for a colour scan with nothing to do to the pixels */
static SANE_Bool
want_passthrough (struct finet_scanner *s)
{
  return !strcmp (s->val[OPT_COMPRESSION].s, compression_list[1])
    && !strcmp (s->val[OPT_MODE].s, SANE_VALUE_SCAN_MODE_COLOR)
    && !s->val[OPT_BRIGHTNESS].w && !s->val[OPT_CONTRAST].w
    && !s->val[OPT_AUTO_SIZE].w;
}

/* The bounding box of the page against a black backing: a line counts as
   content when enough of its pixels are brighter than the backing, so
   speckle on the roller is ignored. Returns 0 with a tight box, or -1 */
static int
content_bbox (const unsigned char *img, int w, int h, int stride,
              int channels, int thr, int *x0, int *y0, int *x1, int *y1)
{
  int x, y, c, minx = w, miny = h, maxx = -1, maxy = -1;
  int row_min = w / 100 + 1, col_min = h / 100 + 1;
  int *colhit = calloc (w, sizeof (int));

  if (!colhit)
    return -1;
  for (y = 0; y < h; y++)
    {
      const unsigned char *p = img + (size_t) y * stride;
      int rowhit = 0;

      for (x = 0; x < w; x++, p += channels)
        {
          int bright = 0;

          for (c = 0; c < channels; c++)
            if (p[c] > thr)
              {
                bright = 1;
                break;
              }
          if (bright)
            {
              rowhit++;
              colhit[x]++;
            }
        }
      if (rowhit >= row_min)
        {
          if (y < miny)
            miny = y;
          if (y > maxy)
            maxy = y;
        }
    }
  for (x = 0; x < w; x++)
    if (colhit[x] >= col_min)
      {
        if (x < minx)
          minx = x;
        if (x > maxx)
          maxx = x;
      }
  free (colhit);
  if (maxx < 0 || maxy < 0)
    return -1;
  *x0 = minx;
  *y0 = miny;
  *x1 = maxx + 1;
  *y1 = maxy + 1;
  return 0;
}

/* When auto-size is on the sheet is scanned against a black backing and
   cropped here to the page it finds, so the scanner need not detect the
   edges itself (unreliable when paper and backing are both pale) */
static void
crop_to_content (struct finet_scanner *s)
{
  int ch = s->params.format == SANE_FRAME_RGB ? 3 : 1;
  int w = s->params.pixels_per_line, h = s->params.lines;
  int stride = s->params.bytes_per_line;
  /* no margin: crop tight to the page so the black backing does not
     show as an edge or count towards the coverage figure */
  int margin = 0;
  int x0, y0, x1, y1, nw, nh, nstride, y;
  unsigned char *out;

  if (content_bbox (s->image, w, h, stride, ch, 40, &x0, &y0, &x1, &y1))
    return;
  x0 = x0 > margin ? x0 - margin : 0;
  y0 = y0 > margin ? y0 - margin : 0;
  x1 = x1 + margin < w ? x1 + margin : w;
  y1 = y1 + margin < h ? y1 + margin : h;
  nw = x1 - x0;
  nh = y1 - y0;
  if (nw >= w && nh >= h)
    return;                     /* nothing to trim */
  nstride = nw * ch;
  for (y = 0; y < nh; y++)
    {
      out = s->image + (size_t) y * nstride;
      memmove (out, s->image + (size_t) (y0 + y) * stride + (size_t) x0 * ch,
               (size_t) nstride);
    }
  s->params.pixels_per_line = nw;
  s->params.lines = nh;
  s->params.bytes_per_line = nstride;
  s->image_size = (size_t) nstride * nh;
  s->last_w = nw;
  s->last_h = nh;
  DBG (10, "cropped to %dx%d at %d,%d\n", nw, nh, x0, y0);
}

/* Hand the scanner's JPEG over as a SANE_FRAME_JPEG, sized as the
   fujitsu backend does: the frame's pixels and the bytes an uncompressed
   image would take */
static SANE_Status
keep_jpeg (struct finet_scanner *s, const unsigned char *jpeg, size_t size,
           int width, int height)
{
  SANE_Bool colour = !strcmp (s->val[OPT_MODE].s, SANE_VALUE_SCAN_MODE_COLOR);

  free (s->image);
  s->image = malloc (size);
  if (!s->image)
    return SANE_STATUS_NO_MEM;
  memcpy (s->image, jpeg, size);
  s->image_size = size;
  s->image_pos = 0;
  s->params.format = SANE_FRAME_JPEG;
  s->params.depth = 8;
  s->params.pixels_per_line = width;
  s->params.lines = height;
  s->params.bytes_per_line = colour ? width * 3 : width;
  s->params.last_frame = SANE_TRUE;
  DBG (10, "passing on %zu byte JPEG, %dx%d\n", size, width, height);
  return SANE_STATUS_GOOD;
}

/* Decode the JPEG into s->image in the requested mode and fill in the
   scan parameters */
static SANE_Status
decode_image (struct finet_scanner *s, const unsigned char *jpeg, size_t size)
{
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error err;
  const char *mode = s->val[OPT_MODE].s;
  SANE_Bool colour = !strcmp (mode, SANE_VALUE_SCAN_MODE_COLOR);
  SANE_Bool lineart = !strcmp (mode, SANE_VALUE_SCAN_MODE_LINEART);
  int threshold = s->val[OPT_THRESHOLD].w;
  int brightness = s->val[OPT_BRIGHTNESS].w;
  int contrast = s->val[OPT_CONTRAST].w;
  unsigned char lut[256];
  SANE_Bool adjust = brightness || contrast;
  unsigned char *row = NULL;
  int width, height, stride, y, i;

  /* brightness shifts, contrast stretches about mid-grey: -127 flattens
     the image, +127 makes it almost two-level */
  for (i = 0; i < 256; i++)
    {
      double v = i;

      if (contrast)
        {
          double gain = contrast > 0 ? 1.0 + contrast / 32.0
            : 1.0 + contrast / 128.0;

          v = 128 + (v - 128) * gain;
        }
      v += brightness;
      lut[i] = v < 0 ? 0 : v > 255 ? 255 : (unsigned char) (v + 0.5);
    }

  cinfo.err = jpeg_std_error (&err.mgr);
  err.mgr.error_exit = jpeg_fail;
  if (setjmp (err.jump))
    {
      DBG (1, "JPEG decode failed\n");
      jpeg_destroy_decompress (&cinfo);
      free (row);
      return SANE_STATUS_IO_ERROR;
    }
  jpeg_create_decompress (&cinfo);
  jpeg_mem_src (&cinfo, (unsigned char *) jpeg, size);
  jpeg_read_header (&cinfo, TRUE);
  cinfo.out_color_space = colour ? JCS_RGB : JCS_GRAYSCALE;
  /* the scan is going to be thresholded or stored lossily anyway, so
     decode for speed: the fast integer DCT and plain upsampling roughly
     halve the decode time with no visible difference */
  cinfo.dct_method = JDCT_IFAST;
  cinfo.do_fancy_upsampling = FALSE;
  jpeg_start_decompress (&cinfo);
  width = cinfo.output_width;
  height = cinfo.output_height;

  s->params.format = colour ? SANE_FRAME_RGB : SANE_FRAME_GRAY;
  s->params.depth = lineart ? 1 : 8;
  s->params.pixels_per_line = width;
  s->params.lines = height;
  s->params.bytes_per_line = colour ? width * 3 : lineart ? (width + 7) / 8
    : width;
  s->params.last_frame = SANE_TRUE;
  stride = s->params.bytes_per_line;

  free (s->image);
  s->image = malloc ((size_t) stride * height);
  row = malloc (cinfo.output_width * cinfo.output_components);
  if (!s->image || !row)
    {
      jpeg_destroy_decompress (&cinfo);
      free (row);
      return SANE_STATUS_NO_MEM;
    }
  s->image_size = (size_t) stride * height;
  s->image_pos = 0;

  for (y = 0; y < height; y++)
    {
      unsigned char *out = s->image + (size_t) y * stride;

      if (lineart)
        {
          int x;

          jpeg_read_scanlines (&cinfo, &row, 1);
          memset (out, 0, stride);
          for (x = 0; x < width; x++)
            if (lut[row[x]] < threshold)
              out[x / 8] |= 0x80 >> (x % 8);
        }
      else
        {
          jpeg_read_scanlines (&cinfo, &out, 1);
          if (adjust)
            for (i = 0; i < stride; i++)
              out[i] = lut[out[i]];
        }
    }
  jpeg_finish_decompress (&cinfo);
  jpeg_destroy_decompress (&cinfo);
  free (row);
  DBG (10, "decoded %dx%d %s\n", width, height, mode);
  /* crop the page out of the black backing (colour and grey only) */
  if (s->val[OPT_AUTO_SIZE].w && !lineart)
    {
      crop_to_content (s);
      s->last_w = s->params.pixels_per_line;
      s->last_h = s->params.lines;
    }
  return SANE_STATUS_GOOD;
}

/* Is the JPEG of this block wanted? The scanner always scans both sides,
   front first, so a single-sided scan drops every other image */
static SANE_Bool
want_block (struct finet_scanner *s, int block)
{
  int source = source_index (s);

  if (source == SOURCE_FRONT)
    return block % 2 == 1;
  if (source == SOURCE_BACK)
    return block % 2 == 0;
  return SANE_TRUE;
}

/* Fetch one image block: wait for the scanner to have it, then get the
   JPEG if it is wanted. Frees the block before it first, which has been
   consumed by now, so that round-trip is off the critical path too. The
   feeder keeps running between images, so the wait ends when the hopper
   runs out or, once the feeder is stopped, when the sheets already fed
   have all come: NO_DOCS then, leaving the session for the caller to
   close. Runs in the background thread as well as directly, so it must
   not touch anything but the connection and the buffers it is given */
static SANE_Status
fetch_block (struct finet_scanner *s, int block, SANE_Bool want,
             struct buffer *meta, struct buffer *jpeg)
{
  char params[192];
  char uri[128], val[32];
  time_t start = time (NULL);
  SANE_Status status;
  int part = 1;
  struct buffer piece = { 0 };

  if (s->to_release)
    {
      snprintf (params, sizeof (params),
                "\"imageBlockNum\":%d,\"sessionId\":\"%s\"", s->to_release,
                s->session);
      command (s, "releaseImageBlocks", params, 60, meta);
      buffer_free (meta);
      s->to_release = 0;
    }

  for (;;)
    {
      snprintf (params, sizeof (params),
                "\"duplexMetadata\":\"disable\",\"imageBlockNum\":%d,"
                "\"imagePartNum\":%d,\"sessionId\":\"%s\","
                "\"withMetadata\":\"enable\"", block, part, s->session);
      /* the scanner holds the request until an image arrives; once the
         feeder is stopped, only sheets already in the paper path can
         still come, so ask first rather than sit out the hold */
      if (s->feed_stopped && block > s->pending_upto && !image_pending (s))
        {
          DBG (10, "feeder stopped and drained: done\n");
          return SANE_STATUS_NO_DOCS;
        }
      status = command (s, "readImageBlock", params, 60, meta);
      if (status != SANE_STATUS_GOOD)
        return status;
      if (!strstr ((const char *) meta->data, "\"status\":\"noImage\""))
        break;

      /* nothing yet: the end of the paper, a stopped feeder, or a slow
         scan */
      buffer_free (meta);
      if (s->feed_stopped)
        {
          DBG (10, "feeder stopped, nothing more came: done\n");
          return SANE_STATUS_NO_DOCS;
        }
      status = check_hopper (s);
      if (status == SANE_STATUS_NO_DOCS)
        DBG (10, "hopper empty: done\n");
      if (status != SANE_STATUS_GOOD)
        return status;
      if (time (NULL) - start > IMAGE_TIMEOUT)
        {
          DBG (1, "no image after %d seconds\n", IMAGE_TIMEOUT);
          return SANE_STATUS_IO_ERROR;
        }
    }
  if (!json_get ((const char *) meta->data, "uri", uri, sizeof (uri)))
    {
      DBG (1, "readImageBlock: no image: %.300s\n", (const char *) meta->data);
      return SANE_STATUS_IO_ERROR;
    }
  DBG (10, "image %d: %s %dx%d, %d bytes\n", block, uri,
       json_get_int ((const char *) meta->data, "pixelWidth", 0),
       json_get_int ((const char *) meta->data, "pixelHeight", 0),
       json_get_int ((const char *) meta->data, "size", 0));

  poll_queue (s, block);
  if (!want)
    return SANE_STATUS_GOOD;

  /* the image may come in parts, the last of them named *_end: fetch
     and join them. At the sizes scanned here it is always one part */
  for (;;)
    {
      status = http (s, uri, NULL, COMMAND_TIMEOUT, &piece);
      if (status != SANE_STATUS_GOOD)
        break;
      if (write_cb (piece.data, 1, piece.size, jpeg) != piece.size)
        {
          status = SANE_STATUS_NO_MEM;
          break;
        }
      if (!(json_get ((const char *) meta->data, "moreParts", val,
                      sizeof (val)) && !strcmp (val, "true"))
          || strstr (uri, "_end."))
        break;
      part++;
      snprintf (params, sizeof (params),
                "\"duplexMetadata\":\"disable\",\"imageBlockNum\":%d,"
                "\"imagePartNum\":%d,\"sessionId\":\"%s\","
                "\"withMetadata\":\"enable\"", block, part, s->session);
      status = command (s, "readImageBlock", params, 60, meta);
      if (status != SANE_STATUS_GOOD)
        break;
      if (!json_get ((const char *) meta->data, "uri", uri, sizeof (uri)))
        {
          DBG (1, "image %d part %d: no uri: %.300s\n", block, part,
               (const char *) meta->data);
          status = SANE_STATUS_IO_ERROR;
          break;
        }
    }
  buffer_free (&piece);
  if (status != SANE_STATUS_GOOD)
    return status;
  /* what arrives must be one whole JPEG, or the frontend's decoder will
     fail in obscure ways later */
  if (jpeg->size < 4 || jpeg->data[0] != 0xff || jpeg->data[1] != 0xd8
      || jpeg->data[jpeg->size - 2] != 0xff
      || jpeg->data[jpeg->size - 1] != 0xd9
      || (part == 1
          && (size_t) json_get_int ((const char *) meta->data, "size", 0)
          != jpeg->size))
    {
      DBG (1, "image %d: got %zu bytes in %d part(s), expected %d; starts "
           "%02x%02x, ends %02x%02x\n", block, jpeg->size, part,
           json_get_int ((const char *) meta->data, "size", 0),
           jpeg->size ? jpeg->data[0] : 0, jpeg->size > 1 ? jpeg->data[1] : 0,
           jpeg->size > 1 ? jpeg->data[jpeg->size - 2] : 0,
           jpeg->size ? jpeg->data[jpeg->size - 1] : 0);
      return SANE_STATUS_IO_ERROR;
    }
  return SANE_STATUS_GOOD;
}

/* Fetching an image and decoding it take about as long as each other,
   and done one after the other they cannot keep up with the feeder, so
   the next block is fetched in the background while this one is decoded.
   The connection is not shared: whoever wants it waits for the fetch */

static void *
prefetch_run (void *arg)
{
  struct finet_scanner *s = arg;

  s->pf.status = fetch_block (s, s->pf.block, s->pf.want, &s->pf.meta,
                              &s->pf.jpeg);
  return NULL;
}

/* wait for a fetch in progress; its result stays for read_image() */
static void
prefetch_wait (struct finet_scanner *s)
{
  if (!s->pf.running)
    return;
  pthread_join (s->pf.thread, NULL);
  s->pf.running = SANE_FALSE;
  s->pf.valid = SANE_TRUE;
}

/* wait for a fetch in progress and discard whatever it got */
static void
prefetch_drop (struct finet_scanner *s)
{
  prefetch_wait (s);
  buffer_free (&s->pf.meta);
  buffer_free (&s->pf.jpeg);
  s->pf.valid = SANE_FALSE;
  s->pf.abort = SANE_FALSE;
}

static void
prefetch_start (struct finet_scanner *s)
{
  if (s->pf.running || s->pf.valid)
    return;
  s->pf.block = s->block;
  s->pf.want = want_block (s, s->block);
  if (pthread_create (&s->pf.thread, NULL, prefetch_run, s))
    {
      DBG (1, "cannot start the fetch thread\n");
      return;
    }
  s->pf.running = SANE_TRUE;
}

/* Get the next image, or with fetch off just let the scanner drop it */
static SANE_Status
read_image (struct finet_scanner *s, SANE_Bool fetch)
{
  struct buffer meta = { 0 };
  struct buffer jpeg = { 0 };
  char uri[128];
  SANE_Status status;

  prefetch_wait (s);
  if (s->pf.valid && s->pf.block == s->block)
    {
      meta = s->pf.meta;
      jpeg = s->pf.jpeg;
      status = s->pf.status;
      memset (&s->pf.meta, 0, sizeof (s->pf.meta));
      memset (&s->pf.jpeg, 0, sizeof (s->pf.jpeg));
      s->pf.valid = SANE_FALSE;
    }
  else
    {
      prefetch_drop (s);
      status = fetch_block (s, s->block, fetch, &meta, &jpeg);
    }
  if (status == SANE_STATUS_NO_DOCS)
    close_session (s);
  if (status != SANE_STATUS_GOOD)
    goto out;

  /* the scanner keeps this block until the next fetch frees it; start
     that now, so its wait overlaps the decode */
  s->to_release = s->block;
  s->block++;
  prefetch_start (s);

  if (!fetch)
    goto out;
  /* not prefetched, or prefetched without the image: get it now */
  if (!jpeg.size)
    {
      if (!json_get ((const char *) meta.data, "uri", uri, sizeof (uri)))
        {
          status = SANE_STATUS_IO_ERROR;
          goto out;
        }
      prefetch_wait (s);
      status = http (s, uri, NULL, COMMAND_TIMEOUT, &jpeg);
      if (status != SANE_STATUS_GOOD)
        goto out;
    }
  if (want_passthrough (s))
    status = keep_jpeg (s, jpeg.data, jpeg.size,
                        json_get_int ((const char *) meta.data, "pixelWidth", 0),
                        json_get_int ((const char *) meta.data, "pixelHeight", 0));
  else
    status = decode_image (s, jpeg.data, jpeg.size);

out:
  buffer_free (&meta);
  buffer_free (&jpeg);
  return status;
}

/* ---------------------------------------------------------------------- */
/* Devices and options */

static SANE_Status
attach (const char *host, int port)
{
  struct finet_device *dev;
  char name[256];

  for (dev = device_list; dev; dev = dev->next)
    if (!strcmp (dev->host, host) && dev->port == port)
      return SANE_STATUS_GOOD;

  dev = calloc (1, sizeof (*dev));
  if (!dev)
    return SANE_STATUS_NO_MEM;
  dev->host = strdup (host);
  dev->port = port;
  /* the dll layer prefixes the backend name itself */
  if (port == FINET_DEFAULT_PORT)
    snprintf (name, sizeof (name), "%s", host);
  else
    snprintf (name, sizeof (name), "%s:%d", host, port);
  dev->name = strdup (name);
  dev->model = strdup ("fi-8000 series");
  dev->sane.name = dev->name;
  dev->sane.vendor = "Ricoh";
  dev->sane.model = dev->model;
  dev->sane.type = "sheetfed scanner";
  dev->next = device_list;
  device_list = dev;
  DBG (5, "attached %s\n", name);
  return SANE_STATUS_GOOD;
}

/* handle one discovery reply: the scanner's IP is at offset 16 and its
   name (like "fi-8950-CLAC001933") at offset 40 */
static void
discovery_reply (const unsigned char *data, size_t len)
{
  char host[16], name[64];

  if (len < 44 || memcmp (data, FINET_MAGIC, 4))
    return;
  snprintf (host, sizeof (host), "%u.%u.%u.%u", data[16], data[17], data[18],
            data[19]);
  size_t n = 0;
  for (size_t i = 40; i < len && data[i] && n < sizeof (name) - 1; i++)
    name[n++] = data[i];
  name[n] = 0;
  DBG (5, "discovered %s (%s)\n", host, name);
  attach (host, FINET_DEFAULT_PORT);
}

/* Broadcast a probe on every interface and collect the replies */
static void
discover (void)
{
  /* the vendor tool's probe: magic, then the sender's IPv4 address at
     offset 8 and MAC at 12, then 0xff 0x02 at 23; the scanner ignores
     anything else */
  unsigned char probe[32] = {
    'f', 'i', 'C', 'H', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0
  };
  struct ifaddrs *ifaddr, *ifa;
  struct sockaddr_in to;
  struct timeval tv;
  int fd, on = 1;

  fd = socket (AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return;
  setsockopt (fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof (on));

  memset (&to, 0, sizeof (to));
  to.sin_family = AF_INET;
  to.sin_port = htons (FINET_DISCOVERY_PORT);

  /* the global broadcast, plus each interface's own broadcast address,
     since some networks drop 255.255.255.255 */
  to.sin_addr.s_addr = INADDR_BROADCAST;
  sendto (fd, probe, sizeof (probe), 0, (struct sockaddr *) &to, sizeof (to));
  if (getifaddrs (&ifaddr) == 0)
    {
      for (ifa = ifaddr; ifa; ifa = ifa->ifa_next)
        {
          struct sockaddr_in *b;

          if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET
              || !ifa->ifa_broadaddr
              || ifa->ifa_broadaddr->sa_family != AF_INET
              || !(ifa->ifa_flags & IFF_BROADCAST))
            continue;
          b = (struct sockaddr_in *) ifa->ifa_addr;
          memcpy (probe + 8, &b->sin_addr.s_addr, 4);
          b = (struct sockaddr_in *) ifa->ifa_broadaddr;
          to.sin_addr = b->sin_addr;
          DBG (15, "probing %s\n", inet_ntoa (b->sin_addr));
          sendto (fd, probe, sizeof (probe), 0, (struct sockaddr *) &to,
                  sizeof (to));
        }
      freeifaddrs (ifaddr);
    }

  tv.tv_sec = FINET_DISCOVERY_MS / 1000;
  tv.tv_usec = (FINET_DISCOVERY_MS % 1000) * 1000;
  setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
  for (;;)
    {
      unsigned char buf[512];
      ssize_t n = recv (fd, buf, sizeof (buf), 0);

      if (n < 0)
        break;
      discovery_reply (buf, (size_t) n);
    }
  close (fd);
}

static SANE_Status
attach_config (SANEI_Config __sane_unused__ * config, const char *line,
               void __sane_unused__ * data)
{
  char host[256];
  int port = FINET_DEFAULT_PORT;
  const char *p = line;
  size_t n = 0;

  if (!strncmp (p, "option", 6))
    {
      if (strstr (line, "no-discovery"))
        discovery_disabled = SANE_TRUE;
      return SANE_STATUS_GOOD;
    }
  if (!strncmp (p, "device", 6))
    p = sanei_config_skip_whitespace (p + 6);
  while (*p && !isspace ((unsigned char) *p) && *p != ':' && n < sizeof (host) - 1)
    host[n++] = *p++;
  host[n] = 0;
  if (*p == ':')
    port = atoi (p + 1);
  if (!n)
    return SANE_STATUS_GOOD;
  return attach (host, port);
}

/* Ask the scanner what it is, to name it properly */
static void
identify (struct finet_scanner *s)
{
  struct buffer buf = { 0 };
  char val[128];

  if (get_info (s, &buf) == SANE_STATUS_GOOD)
    {
      const char *json = (const char *) buf.data;

      if (json_get (json, "model", val, sizeof (val)))
        {
          free (s->dev->model);
          s->dev->model = strdup (val);
          s->dev->sane.model = s->dev->model;
        }
      if (json_get (json, "serialNumber", val, sizeof (val)))
        {
          free (s->dev->serial);
          s->dev->serial = strdup (val);
        }
      DBG (5, "%s: model %s serial %s state %s\n", s->dev->name,
           s->dev->model, s->dev->serial ? s->dev->serial : "?",
           json_get (json, "deviceState", val, sizeof (val)) ? val : "?");
    }
  buffer_free (&buf);
}

static void
init_options (struct finet_scanner *s)
{
  SANE_Option_Descriptor *o;
  int i;

  memset (s->opt, 0, sizeof (s->opt));
  memset (s->val, 0, sizeof (s->val));
  for (i = 0; i < NUM_OPTIONS; i++)
    {
      s->opt[i].size = sizeof (SANE_Word);
      s->opt[i].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    }

  x_range.min = 0;
  x_range.max = SANE_FIX (MAX_WIDTH_MM);
  x_range.quant = 0;
  y_range.min = 0;
  y_range.max = SANE_FIX (MAX_HEIGHT_MM);
  y_range.quant = 0;

  o = &s->opt[OPT_NUM_OPTS];
  o->name = SANE_NAME_NUM_OPTIONS;
  o->title = SANE_TITLE_NUM_OPTIONS;
  o->desc = SANE_DESC_NUM_OPTIONS;
  o->type = SANE_TYPE_INT;
  o->cap = SANE_CAP_SOFT_DETECT;
  s->val[OPT_NUM_OPTS].w = NUM_OPTIONS;

  o = &s->opt[OPT_STANDARD_GROUP];
  o->name = SANE_NAME_STANDARD;
  o->title = SANE_TITLE_STANDARD;
  o->desc = SANE_DESC_STANDARD;
  o->type = SANE_TYPE_GROUP;
  o->cap = 0;

  o = &s->opt[OPT_SOURCE];
  o->name = SANE_NAME_SCAN_SOURCE;
  o->title = SANE_TITLE_SCAN_SOURCE;
  o->desc = SANE_DESC_SCAN_SOURCE;
  o->type = SANE_TYPE_STRING;
  o->constraint_type = SANE_CONSTRAINT_STRING_LIST;
  o->constraint.string_list = source_list;
  o->size = 16;
  s->val[OPT_SOURCE].s = strdup (source_list[0]);

  o = &s->opt[OPT_MODE];
  o->name = SANE_NAME_SCAN_MODE;
  o->title = SANE_TITLE_SCAN_MODE;
  o->desc = SANE_DESC_SCAN_MODE;
  o->type = SANE_TYPE_STRING;
  o->constraint_type = SANE_CONSTRAINT_STRING_LIST;
  o->constraint.string_list = mode_list;
  o->size = 16;
  s->val[OPT_MODE].s = strdup (SANE_VALUE_SCAN_MODE_LINEART);

  o = &s->opt[OPT_RESOLUTION];
  o->name = SANE_NAME_SCAN_RESOLUTION;
  o->title = SANE_TITLE_SCAN_RESOLUTION;
  o->desc = SANE_DESC_SCAN_RESOLUTION;
  o->type = SANE_TYPE_INT;
  o->unit = SANE_UNIT_DPI;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &resolution_range;
  s->val[OPT_RESOLUTION].w = 300;

  o = &s->opt[OPT_GEOMETRY_GROUP];
  o->name = SANE_NAME_GEOMETRY;
  o->title = SANE_TITLE_GEOMETRY;
  o->desc = SANE_DESC_GEOMETRY;
  o->type = SANE_TYPE_GROUP;
  o->cap = 0;

  o = &s->opt[OPT_AUTO_SIZE];
  o->name = "auto-size";
  o->title = SANE_I18N ("Automatic size");
  o->desc = SANE_I18N ("Let the scanner detect each sheet's size and crop "
                       "to it, instead of using the scan area");
  o->type = SANE_TYPE_BOOL;
  s->val[OPT_AUTO_SIZE].w = SANE_FALSE;

  o = &s->opt[OPT_TL_X];
  o->name = SANE_NAME_SCAN_TL_X;
  o->title = SANE_TITLE_SCAN_TL_X;
  o->desc = SANE_DESC_SCAN_TL_X;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &x_range;
  s->val[OPT_TL_X].w = 0;

  o = &s->opt[OPT_TL_Y];
  o->name = SANE_NAME_SCAN_TL_Y;
  o->title = SANE_TITLE_SCAN_TL_Y;
  o->desc = SANE_DESC_SCAN_TL_Y;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &y_range;
  s->val[OPT_TL_Y].w = 0;

  o = &s->opt[OPT_BR_X];
  o->name = SANE_NAME_SCAN_BR_X;
  o->title = SANE_TITLE_SCAN_BR_X;
  o->desc = SANE_DESC_SCAN_BR_X;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &x_range;
  s->val[OPT_BR_X].w = SANE_FIX (210.0);

  o = &s->opt[OPT_BR_Y];
  o->name = SANE_NAME_SCAN_BR_Y;
  o->title = SANE_TITLE_SCAN_BR_Y;
  o->desc = SANE_DESC_SCAN_BR_Y;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &y_range;
  s->val[OPT_BR_Y].w = SANE_FIX (297.0);

  o = &s->opt[OPT_PAGE_WIDTH];
  o->name = SANE_NAME_PAGE_WIDTH;
  o->title = SANE_TITLE_PAGE_WIDTH;
  o->desc = SANE_DESC_PAGE_WIDTH;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &x_range;
  s->val[OPT_PAGE_WIDTH].w = SANE_FIX (210.0);

  o = &s->opt[OPT_PAGE_HEIGHT];
  o->name = SANE_NAME_PAGE_HEIGHT;
  o->title = SANE_TITLE_PAGE_HEIGHT;
  o->desc = SANE_DESC_PAGE_HEIGHT;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &y_range;
  s->val[OPT_PAGE_HEIGHT].w = SANE_FIX (297.0);

  o = &s->opt[OPT_ENHANCEMENT_GROUP];
  o->name = SANE_NAME_ENHANCEMENT;
  o->title = SANE_TITLE_ENHANCEMENT;
  o->desc = SANE_DESC_ENHANCEMENT;
  o->type = SANE_TYPE_GROUP;
  o->cap = 0;

  o = &s->opt[OPT_BRIGHTNESS];
  o->name = SANE_NAME_BRIGHTNESS;
  o->title = SANE_TITLE_BRIGHTNESS;
  o->desc = SANE_DESC_BRIGHTNESS;
  o->type = SANE_TYPE_INT;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &level_range;
  s->val[OPT_BRIGHTNESS].w = 0;

  o = &s->opt[OPT_CONTRAST];
  o->name = SANE_NAME_CONTRAST;
  o->title = SANE_TITLE_CONTRAST;
  o->desc = SANE_DESC_CONTRAST;
  o->type = SANE_TYPE_INT;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &level_range;
  s->val[OPT_CONTRAST].w = 0;

  o = &s->opt[OPT_THRESHOLD];
  o->name = SANE_NAME_THRESHOLD;
  o->title = SANE_TITLE_THRESHOLD;
  o->desc = SANE_DESC_THRESHOLD;
  o->type = SANE_TYPE_INT;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &threshold_range;
  s->val[OPT_THRESHOLD].w = 128;

  o = &s->opt[OPT_DESKEW];
  o->name = "deskew";
  o->title = SANE_I18N ("Deskew");
  o->desc = SANE_I18N ("Let the scanner straighten skewed pages");
  o->type = SANE_TYPE_BOOL;
  s->val[OPT_DESKEW].w = SANE_FALSE;

  o = &s->opt[OPT_ADVANCED_GROUP];
  o->name = SANE_NAME_ADVANCED;
  o->title = SANE_TITLE_ADVANCED;
  o->desc = SANE_DESC_ADVANCED;
  o->type = SANE_TYPE_GROUP;
  o->cap = 0;

  o = &s->opt[OPT_COMPRESSION];
  o->name = "compression";
  o->title = SANE_I18N ("Compression");
  o->desc = SANE_I18N ("Hand over the scanner's JPEG rather than decoding it "
                       "(colour only)");
  o->type = SANE_TYPE_STRING;
  o->constraint_type = SANE_CONSTRAINT_STRING_LIST;
  o->constraint.string_list = compression_list;
  o->size = 8;
  s->val[OPT_COMPRESSION].s = strdup (compression_list[0]);

  o = &s->opt[OPT_JPEG_QUALITY];
  o->name = "jpeg-quality";
  o->title = SANE_I18N ("JPEG quality");
  o->desc = SANE_I18N ("Quality of the JPEG the scanner sends, 1 to 100");
  o->type = SANE_TYPE_INT;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &quality_range;
  s->val[OPT_JPEG_QUALITY].w = 80;

  o = &s->opt[OPT_BG_COLOUR];
  o->name = "bg-color";
  o->title = SANE_I18N ("Background colour");
  o->desc = SANE_I18N ("Colour of the scanner's backing, seen beyond the "
                       "edges of the paper");
  o->type = SANE_TYPE_STRING;
  o->constraint_type = SANE_CONSTRAINT_STRING_LIST;
  o->constraint.string_list = bg_colour_list;
  o->size = 8;
  s->val[OPT_BG_COLOUR].s = strdup (bg_colour_list[0]);

  o = &s->opt[OPT_DF_DETECT];
  o->name = "df-detect";
  o->title = SANE_I18N ("Double feed detection");
  o->desc = SANE_I18N ("Stop when the scanner's ultrasonic sensor finds "
                       "two sheets feeding at once");
  o->type = SANE_TYPE_BOOL;
  s->val[OPT_DF_DETECT].w = SANE_TRUE;

  o = &s->opt[OPT_PREPICK];
  o->name = "prepick";
  o->title = SANE_I18N ("Pre-pick");
  o->desc = SANE_I18N ("Feed the next sheet to the pick position before it "
                       "is needed, for speed");
  o->type = SANE_TYPE_BOOL;
  s->val[OPT_PREPICK].w = SANE_TRUE;

  /* the one option that may be set during a scan: a frontend presses it
     to end a batch early without losing the sheets already fed */
  o = &s->opt[OPT_STOP_FEED];
  o->name = "stop-feed";
  o->title = SANE_I18N ("Stop feeding");
  o->desc = SANE_I18N ("Stop the feeder but keep the pages already "
                       "scanned, which are read out before the batch ends");
  o->type = SANE_TYPE_BUTTON;
  o->size = 0;
  o->cap |= SANE_CAP_ADVANCED;

  /* read-only: how far the scanner is ahead of the frontend */
  o = &s->opt[OPT_IMAGES_WAITING];
  o->name = "images-waiting";
  o->title = SANE_I18N ("Images waiting");
  o->desc = SANE_I18N ("Images the scanner has finished but has not yet "
                       "handed over");
  o->type = SANE_TYPE_INT;
  o->cap = SANE_CAP_SOFT_DETECT | SANE_CAP_ADVANCED;
  s->val[OPT_IMAGES_WAITING].w = 0;
}

/* Guess the parameters before a scan starts, from the options */
static void
estimate_params (struct finet_scanner *s)
{
  const char *mode = s->val[OPT_MODE].s;
  SANE_Bool colour = !strcmp (mode, SANE_VALUE_SCAN_MODE_COLOR);
  SANE_Bool lineart = !strcmp (mode, SANE_VALUE_SCAN_MODE_LINEART);
  int res = s->val[OPT_RESOLUTION].w;
  SANE_Bool autosize = s->val[OPT_AUTO_SIZE].w;
  /* under auto-size the real size is only known once a sheet is scanned;
     estimate it as the declared paper size rather than the whole bed */
  double wmm = autosize ? SANE_UNFIX (s->val[OPT_PAGE_WIDTH].w)
    : SANE_UNFIX (s->val[OPT_BR_X].w - s->val[OPT_TL_X].w);
  double hmm = autosize ? SANE_UNFIX (s->val[OPT_PAGE_HEIGHT].w)
    : SANE_UNFIX (s->val[OPT_BR_Y].w - s->val[OPT_TL_Y].w);
  int width = (int) (wmm / MM_PER_INCH * res + 0.5);
  int height = (int) (hmm / MM_PER_INCH * res + 0.5);

  if (autosize && s->last_w && s->last_h)
    {
      width = s->last_w;
      height = s->last_h;
    }

  s->params.format = want_passthrough (s) ? SANE_FRAME_JPEG
    : colour ? SANE_FRAME_RGB : SANE_FRAME_GRAY;
  s->params.depth = lineart ? 1 : 8;
  s->params.pixels_per_line = width;
  s->params.lines = height;
  s->params.bytes_per_line = colour ? width * 3 : lineart ? (width + 7) / 8
    : width;
  s->params.last_frame = SANE_TRUE;
}

/* ---------------------------------------------------------------------- */
/* The SANE API */

SANE_Status
sane_init (SANE_Int * version_code, SANE_Auth_Callback __sane_unused__ authorize)
{
  DBG_INIT ();
  DBG (2, "sane_init: finet backend build %d\n", BUILD);
  if (version_code)
    *version_code = SANE_VERSION_CODE (SANE_CURRENT_MAJOR, SANE_CURRENT_MINOR,
                                       BUILD);
  curl_global_init (CURL_GLOBAL_ALL);
  sanei_configure_attach (FINET_CONFIG_FILE, NULL, attach_config, NULL);
  return SANE_STATUS_GOOD;
}

void
sane_exit (void)
{
  struct finet_device *dev;

  while (scanner_list)
    sane_close (scanner_list);
  while ((dev = device_list))
    {
      device_list = dev->next;
      free (dev->host);
      free (dev->name);
      free (dev->model);
      free (dev->serial);
      free (dev);
    }
  free (sane_device_list);
  sane_device_list = NULL;
  curl_global_cleanup ();
}

SANE_Status
sane_get_devices (const SANE_Device *** device_list_out,
                  SANE_Bool __sane_unused__ local_only)
{
  struct finet_device *dev;
  int count = 0, i = 0;

  if (!discovery_disabled)
    discover ();

  for (dev = device_list; dev; dev = dev->next)
    count++;
  free (sane_device_list);
  sane_device_list = calloc (count + 1, sizeof (*sane_device_list));
  if (!sane_device_list)
    return SANE_STATUS_NO_MEM;
  for (dev = device_list; dev; dev = dev->next)
    sane_device_list[i++] = &dev->sane;
  sane_device_list[i] = NULL;
  *device_list_out = sane_device_list;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_open (SANE_String_Const name, SANE_Handle * handle)
{
  struct finet_device *dev = NULL;
  struct finet_scanner *s;

  DBG (10, "sane_open: %s\n", name);
  if (name && *name)
    {
      /* a name from the list, or an address given directly */
      if (!strncmp (name, "finet:", 6))
        name += 6;
      for (dev = device_list; dev; dev = dev->next)
        if (!strcmp (dev->name, name))
          break;
      if (!dev && attach_config (NULL, name, NULL) == SANE_STATUS_GOOD
          && device_list && !strcmp (device_list->name, name))
        dev = device_list;
    }
  else
    dev = device_list;
  if (!dev)
    return SANE_STATUS_INVAL;

  s = calloc (1, sizeof (*s));
  if (!s)
    return SANE_STATUS_NO_MEM;
  s->dev = dev;
  s->curl = curl_easy_init ();
  if (!s->curl)
    {
      free (s);
      return SANE_STATUS_NO_MEM;
    }
  init_options (s);
  estimate_params (s);
  identify (s);
  s->next = scanner_list;
  scanner_list = s;
  *handle = s;
  return SANE_STATUS_GOOD;
}

void
sane_close (SANE_Handle handle)
{
  struct finet_scanner *s = handle;
  struct finet_scanner **p;

  DBG (10, "sane_close\n");
  close_session (s);
  for (p = &scanner_list; *p; p = &(*p)->next)
    if (*p == s)
      {
        *p = s->next;
        break;
      }
  curl_easy_cleanup (s->curl);
  free (s->image);
  free (s->val[OPT_SOURCE].s);
  free (s->val[OPT_MODE].s);
  free (s->val[OPT_COMPRESSION].s);
  free (s);
}

const SANE_Option_Descriptor *
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
  struct finet_scanner *s = handle;

  if (option < 0 || option >= NUM_OPTIONS)
    return NULL;
  return &s->opt[option];
}

SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option, SANE_Action action,
                     void *value, SANE_Int * info)
{
  struct finet_scanner *s = handle;
  SANE_Option_Descriptor *o;
  SANE_Status status;

  if (info)
    *info = 0;
  if (option < 0 || option >= NUM_OPTIONS)
    return SANE_STATUS_INVAL;
  o = &s->opt[option];
  if (o->type == SANE_TYPE_GROUP)
    return SANE_STATUS_INVAL;

  if (action == SANE_ACTION_GET_VALUE)
    {
      if (o->type == SANE_TYPE_BUTTON)
        return SANE_STATUS_GOOD;
      if (o->type == SANE_TYPE_STRING)
        strcpy (value, s->val[option].s);
      else
        *(SANE_Word *) value = s->val[option].w;
      return SANE_STATUS_GOOD;
    }
  if (action != SANE_ACTION_SET_VALUE)
    return SANE_STATUS_UNSUPPORTED;
  if (!(o->cap & SANE_CAP_SOFT_SELECT))
    return SANE_STATUS_INVAL;
  if (option == OPT_STOP_FEED)
    return stop_feed (s);
  if (s->capturing)
    return SANE_STATUS_DEVICE_BUSY;

  status = sanei_constrain_value (o, value, info);
  if (status != SANE_STATUS_GOOD)
    return status;

  if (o->type == SANE_TYPE_STRING)
    {
      if (strcmp (s->val[option].s, value))
        {
          free (s->val[option].s);
          s->val[option].s = strdup (value);
          if (info)
            *info |= SANE_INFO_RELOAD_PARAMS;
        }
    }
  else
    {
      SANE_Word w = *(SANE_Word *) value;

      if (s->val[option].w != w)
        {
          s->val[option].w = w;
          if (info && option != OPT_THRESHOLD && option != OPT_JPEG_QUALITY
              && option != OPT_BRIGHTNESS && option != OPT_CONTRAST
              && option != OPT_DESKEW && option != OPT_DF_DETECT
              && option != OPT_PREPICK)
            *info |= SANE_INFO_RELOAD_PARAMS;
          if (option == OPT_AUTO_SIZE)
            {
              set_geometry_active (s, !w);
              if (info)
                *info |= SANE_INFO_RELOAD_OPTIONS;
            }
        }
      /* the scan window follows the paper, as the fujitsu backend does */
      if (option == OPT_PAGE_WIDTH && s->val[OPT_BR_X].w > w)
        s->val[OPT_BR_X].w = w;
      if (option == OPT_PAGE_HEIGHT && s->val[OPT_BR_Y].w > w)
        s->val[OPT_BR_Y].w = w;
    }
  estimate_params (s);
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters * params)
{
  struct finet_scanner *s = handle;

  *params = s->params;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_start (SANE_Handle handle)
{
  struct finet_scanner *s = handle;
  int source = source_index (s);
  SANE_Status status;

  DBG (10, "sane_start\n");
  s->cancelled = SANE_FALSE;
  s->eof_pending = SANE_FALSE;
  s->image_pos = 0;
  /* sane_cancel() sets the abort flag and relies on close_session() to
     clear it, which it does not do when there is no session left to
     close, as at the end of a batch. Clear it here, since nothing this
     start does should be abandoned by a cancel that has already been
     dealt with */
  s->pf.abort = SANE_FALSE;

  /* the first sheet of a batch starts the feeder, which then runs
     until the hopper is empty or the scan is cancelled */
  if (!s->capturing)
    {
      status = open_session (s);
      if (status != SANE_STATUS_GOOD)
        return status;
      status = check_hopper (s);
      if (status != SANE_STATUS_GOOD)
        return status;
      status = send_task (s);
      if (status != SANE_STATUS_GOOD)
        return status;
      status = start_capturing (s);
      if (status != SANE_STATUS_GOOD)
        return status;
      s->block = 1;
      s->to_release = 0;
      s->last_w = s->last_h = 0;
    }

  /* the scanner always scans both sides, front first, so single-sided
     scans drop the other image; duplex takes them in turn */
  if (source == SOURCE_BACK)
    {
      status = read_image (s, SANE_FALSE);
      if (status != SANE_STATUS_GOOD)
        return status;
    }
  status = read_image (s, SANE_TRUE);
  if (status == SANE_STATUS_GOOD && source == SOURCE_FRONT)
    status = read_image (s, SANE_FALSE);
  return status;
}

SANE_Status
sane_read (SANE_Handle handle, SANE_Byte * data, SANE_Int max_length,
           SANE_Int * length)
{
  struct finet_scanner *s = handle;
  size_t count;

  *length = 0;
  if (s->cancelled)
    return SANE_STATUS_CANCELLED;
  if (!s->image)
    return SANE_STATUS_INVAL;
  if (s->image_pos >= s->image_size)
    return SANE_STATUS_EOF;
  count = s->image_size - s->image_pos;
  if (count > (size_t) max_length)
    count = max_length;
  memcpy (data, s->image + s->image_pos, count);
  s->image_pos += count;
  *length = count;
  return SANE_STATUS_GOOD;
}

void
sane_cancel (SANE_Handle handle)
{
  struct finet_scanner *s = handle;

  DBG (10, "sane_cancel\n");
  s->cancelled = SANE_TRUE;
  s->pf.abort = SANE_TRUE;        /* cut short a fetch in progress */
  /* the end of the batch, or a real cancel: either way stop the feeder
     and let other users at the scanner */
  close_session (s);
}

SANE_Status
sane_set_io_mode (SANE_Handle __sane_unused__ handle, SANE_Bool non_blocking)
{
  return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

SANE_Status
sane_get_select_fd (SANE_Handle __sane_unused__ handle,
                    SANE_Int __sane_unused__ * fd)
{
  return SANE_STATUS_UNSUPPORTED;
}
