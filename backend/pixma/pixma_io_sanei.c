/* SANE - Scanner Access Now Easy.
 * For limitations, see function sanei_usb_get_vendor_product().

   Copyright (C) 2011-2020 Rolf Bensch <rolf at bensch hyphen online dot de>
   Copyright (C) 2006-2007 Wittawat Yamwong <wittawat@web.de>

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
 */
#include "../include/sane/config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>		/* INT_MAX */
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#if HAVE_LIBCURL
#include <curl/curl.h>
#endif

#if HAVE_LIBSNMP
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#endif

#include "pixma_rename.h"
#include "pixma_common.h"
#include "pixma_io.h"
#include "pixma_bjnp.h"

#include "../include/sane/sanei_usb.h"
#include "../include/sane/sane.h"


#ifdef __GNUC__
# define UNUSED(v) (void) v
#else
# define UNUSED(v)
#endif


struct pixma_io_t
{
  pixma_io_t *next;
  int interface;
  SANE_Int dev;
#if HAVE_LIBCURL
  char *http_url;
  CURL *http_curl;
  struct curl_slist *http_headers;
  unsigned char *http_data;
  size_t http_len;
  size_t http_pos;
#endif
};

typedef struct scanner_info_t
{
  struct scanner_info_t *next;
  char *devname;
  int interface;
  const pixma_config_t *cfg;
  char serial[PIXMA_MAX_ID_LEN + 1];	/* "xxxxyyyy_zzzzzzz..."
					   x = vid, y = pid, z = serial */
} scanner_info_t;

#define INT_USB 0
#define INT_BJNP 1
#define INT_CANON_HTTP 2
#define CANON_SNMP_DISCOVERY_TIMEOUT_MS 500

static scanner_info_t *first_scanner = NULL;
static pixma_io_t *first_io = NULL;
static unsigned nscanners;

#if HAVE_LIBCURL
static size_t
canon_http_write_cb (void *ptr, size_t size, size_t nmemb, void *userdata)
{
  pixma_io_t *io = userdata;
  size_t len = size * nmemb;
  unsigned char *data = realloc (io->http_data, io->http_len + len);
  if (data == NULL)
    return 0;
  io->http_data = data;
  memcpy (io->http_data + io->http_len, ptr, len);
  io->http_len += len;
  return len;
}

static int
canon_http_request (pixma_io_t *io, const void *body, size_t body_len,
                    int get)
{
  CURLcode result;
  long response_code;

  free (io->http_data);
  io->http_data = NULL;
  io->http_len = io->http_pos = 0;
  if (io->http_curl == NULL)
    {
      io->http_curl = curl_easy_init ();
      if (io->http_curl == NULL)
        return PIXMA_ENOMEM;
      io->http_headers = curl_slist_append
        (io->http_headers, "Content-Type: application/octet-stream");
      io->http_headers = curl_slist_append
        (io->http_headers, "X-CHMP-Version: 1.0.0");
    }
  curl_easy_setopt (io->http_curl, CURLOPT_URL, io->http_url);
  curl_easy_setopt (io->http_curl, CURLOPT_HTTPHEADER, io->http_headers);
  curl_easy_setopt (io->http_curl, CURLOPT_WRITEFUNCTION, canon_http_write_cb);
  curl_easy_setopt (io->http_curl, CURLOPT_WRITEDATA, io);
  curl_easy_setopt (io->http_curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt (io->http_curl, CURLOPT_TIMEOUT_MS, 30000L);
  curl_easy_setopt (io->http_curl, CURLOPT_TCP_KEEPALIVE, 1L);
  if (get)
    {
      curl_easy_setopt (io->http_curl, CURLOPT_POST, 0L);
      curl_easy_setopt (io->http_curl, CURLOPT_HTTPGET, 1L);
    }
  else
    {
      curl_easy_setopt (io->http_curl, CURLOPT_HTTPGET, 0L);
      curl_easy_setopt (io->http_curl, CURLOPT_POST, 1L);
      curl_easy_setopt (io->http_curl, CURLOPT_POSTFIELDS, body);
      curl_easy_setopt (io->http_curl, CURLOPT_POSTFIELDSIZE, (long) body_len);
    }
  result = curl_easy_perform (io->http_curl);
  if (result != CURLE_OK ||
      curl_easy_getinfo (io->http_curl, CURLINFO_RESPONSE_CODE,
                         &response_code) != CURLE_OK ||
      response_code < 200 || response_code >= 300)
    return PIXMA_EIO;
  return 0;
}
#endif


static scanner_info_t *
get_scanner_info (unsigned devnr)
{
  scanner_info_t *si;
  for (si = first_scanner; si && devnr != 0; --devnr, si = si->next)
    {
    }
  return si;
}

static SANE_Status
attach (SANE_String_Const devname)
{
  scanner_info_t *si;

  si = (scanner_info_t *) calloc (1, sizeof (*si));
  if (!si)
    return SANE_STATUS_NO_MEM;
  si->devname = strdup (devname);
  if (!si->devname)
    {
      free (si);
      return SANE_STATUS_NO_MEM;
    }
  si -> interface = INT_USB;
  si->next = first_scanner;
  first_scanner = si;
  nscanners++;
  return SANE_STATUS_GOOD;
}


static SANE_Status
attach_bjnp (SANE_String_Const devname,
             SANE_String_Const serial,
             const struct pixma_config_t *cfg)
{
  scanner_info_t *si;

  si = (scanner_info_t *) calloc (1, sizeof (*si));
  if (!si)
    return SANE_STATUS_NO_MEM;
  si->devname = strdup (devname);
  if (!si->devname)
    {
      free (si);
      return SANE_STATUS_NO_MEM;
    }

  si->cfg = cfg;
  snprintf(si->serial, sizeof(si->serial), "%s_%s", cfg->model, serial);
  si -> interface = INT_BJNP;
  si->next = first_scanner;
  first_scanner = si;
  nscanners++;
  return SANE_STATUS_GOOD;
}

static const pixma_config_t *
find_network_config (const char *model,
                     const pixma_config_t *const pixma_devices[])
{
  unsigned i;
  const pixma_config_t *cfg, *best = NULL;
  for (i = 0; pixma_devices[i] != NULL; i++)
    for (cfg = pixma_devices[i]; cfg->name != NULL; cfg++)
      if (strcasestr (model, cfg->model) != NULL)
        if (best == NULL || strlen (cfg->model) > strlen (best->model))
          best = cfg;
  return best;
}

static int
network_scanner_exists (const char *address)
{
  scanner_info_t *si;
  char canon_name[INET_ADDRSTRLEN + sizeof ("canonhttp://")];
  const char *bjnp_prefix = "bjnp://";
  size_t bjnp_prefix_len = strlen (bjnp_prefix);
  size_t address_len = strlen (address);

  snprintf (canon_name, sizeof (canon_name), "canonhttp://%s", address);
  for (si = first_scanner; si != NULL; si = si->next)
    {
      if (strcmp (si->devname, canon_name) == 0)
        return 1;
      if (strncmp (si->devname, bjnp_prefix, bjnp_prefix_len) == 0 &&
          strlen (si->devname) > bjnp_prefix_len + address_len &&
          strncmp (si->devname + bjnp_prefix_len, address,
                   address_len) == 0 &&
          si->devname[bjnp_prefix_len + address_len] == ':')
        return 1;
    }
  return 0;
}

static SANE_Status
attach_canon_http (const char *address, const char *model,
                   const char *serial,
                   const pixma_config_t *const pixma_devices[])
{
  scanner_info_t *si;
  const pixma_config_t *cfg = find_network_config (model, pixma_devices);
  char devname[256];

  if (cfg == NULL)
    return SANE_STATUS_INVAL;
  if (network_scanner_exists (address))
    return SANE_STATUS_GOOD;
  snprintf (devname, sizeof (devname), "canonhttp://%s", address);
  si = calloc (1, sizeof (*si));
  if (si == NULL)
    return SANE_STATUS_NO_MEM;
  si->devname = strdup (devname);
  if (si->devname == NULL)
    {
      free (si);
      return SANE_STATUS_NO_MEM;
    }
  si->cfg = cfg;
  snprintf (si->serial, sizeof (si->serial), "%s_%s", cfg->model, serial);
  si->interface = INT_CANON_HTTP;
  si->next = first_scanner;
  first_scanner = si;
  nscanners++;
  return SANE_STATUS_GOOD;
}

#if HAVE_LIBSNMP
typedef struct
{
  const pixma_config_t *const *pixma_devices;
} canon_snmp_context_t;

static int
canon_snmp_callback (int operation, netsnmp_session *session, int reqid,
                     netsnmp_pdu *pdu, void *magic)
{
  canon_snmp_context_t *context = magic;
  netsnmp_variable_list *var;
  oid model_oid[MAX_OID_LEN];
  oid id_oid[MAX_OID_LEN];
  oid mac_oid[MAX_OID_LEN];
  size_t model_oid_len = MAX_OID_LEN;
  size_t id_oid_len = MAX_OID_LEN;
  size_t mac_oid_len = MAX_OID_LEN;
  char model[128] = "";
  char address[INET_ADDRSTRLEN] = "";
  char serial[64] = "snmp";
  char mac[18] = "";
  netsnmp_indexed_addr_pair *peer;
  struct sockaddr_in *remote;

  UNUSED (session);
  UNUSED (reqid);
  if (operation != NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE || pdu == NULL ||
      !read_objid (".1.3.6.1.4.1.1602.1.1.1.1.0", model_oid,
                   &model_oid_len) ||
      !read_objid (".1.3.6.1.4.1.2699.1.2.1.2.1.1.3.1", id_oid,
                   &id_oid_len) ||
      !read_objid (".1.3.6.1.4.1.1602.1.3.1.13.0", mac_oid,
                   &mac_oid_len))
    return 1;
  for (var = pdu->variables; var != NULL; var = var->next_variable)
    if (var->type == ASN_OCTET_STR && var->val.string != NULL)
      {
        char value[128];
        size_t len = var->val_len < sizeof (value) - 1
                   ? var->val_len : sizeof (value) - 1;
        memcpy (value, var->val.string, len);
        value[len] = '\0';
        if (snmp_oid_compare (var->name, var->name_length,
                              model_oid, model_oid_len) == 0)
          snprintf (model, sizeof (model), "%s", value);
        else if (snmp_oid_compare (var->name, var->name_length,
                                   mac_oid, mac_oid_len) == 0 &&
                 var->val_len == 6)
          snprintf (mac, sizeof (mac), "%02x%02x_%02x%02x_%02x%02x",
                    var->val.string[0], var->val.string[1],
                    var->val.string[2], var->val.string[3],
                    var->val.string[4], var->val.string[5]);
        else if (snmp_oid_compare (var->name, var->name_length,
                                   id_oid, id_oid_len) == 0)
          {
            size_t serial_len = strlen (value);
            if (serial_len >= sizeof (serial))
              serial_len = sizeof (serial) - 1;
            memcpy (serial, value, serial_len);
            serial[serial_len] = '\0';
          }
        else if (strstr (value, "MFG:Canon") != NULL &&
                 strstr (value, "MDL:") != NULL)
          {
            const char *model_start = strstr (value, "MDL:") + 4;
            const char *model_end = strchr (model_start, ';');
            size_t model_len = model_end != NULL
                             ? (size_t) (model_end - model_start)
                             : strlen (model_start);
            if (model_len >= sizeof (model))
              model_len = sizeof (model) - 1;
            memcpy (model, model_start, model_len);
            model[model_len] = '\0';
          }
      }
  if (mac[0] != '\0')
    snprintf (serial, sizeof (serial), "%s", mac);
  peer = pdu->transport_data;
  if (peer == NULL || pdu->transport_data_length != sizeof (*peer))
    return 1;
  remote = (struct sockaddr_in *) &peer->remote_addr;
  if (remote->sin_family != AF_INET)
    return 1;
  inet_ntop (AF_INET, &remote->sin_addr, address, sizeof (address));
  if (model[0] != '\0' && address[0] != '\0')
    {
      PDBG (pixma_dbg (3, "SNMP found Canon %s at %s\n", model, address));
      attach_canon_http (address, model, serial, context->pixma_devices);
    }
  return 1;
}

static void
canon_snmp_discover (const pixma_config_t *const pixma_devices[])
{
  netsnmp_session session, *ss;
  netsnmp_pdu *request = NULL;
  static const char *const discovery_oids[] = {
    ".1.3.6.1.4.1.1602.1.3.1.13.0",
    ".1.3.6.1.4.1.1602.1.2.1.8.1.3.1.1",
    ".1.3.6.1.4.1.1602.1.1.1.1.0",
    ".1.3.6.1.4.1.1602.1.1.1.10.0",
    ".1.3.6.1.4.1.1602.1.3.1.12.0",
    ".1.3.6.1.4.1.2699.1.2.1.2.1.1.3.1"
  };
  oid parsed_oid[MAX_OID_LEN];
  size_t parsed_oid_len;
  unsigned oid_index;
  canon_snmp_context_t context = { pixma_devices };
  char *broadcast = (char *) "255.255.255.255";

  PDBG (pixma_dbg (4, "starting Canon SNMP discovery\n"));

  PDBG (pixma_dbg (4, "Canon SNMP broadcast target: %s\n", broadcast));

  snmp_sess_init (&session);
  session.version = SNMP_VERSION_1;
  session.peername = broadcast;
  session.community = (u_char *) "canon_admin";
  session.community_len = strlen ((char *) session.community);
  session.flags |= SNMP_FLAGS_UDP_BROADCAST;
  session.callback = canon_snmp_callback;
  session.callback_magic = &context;
  SOCK_STARTUP;
  ss = snmp_open (&session);
  if (ss == NULL)
    {
      PDBG (pixma_dbg (2, "Canon SNMP session could not be opened\n"));
      snmp_sess_perror ("canon SNMP", &session);
      SOCK_CLEANUP;
      return;
    }
  request = snmp_pdu_create (SNMP_MSG_GET);
  for (oid_index = 0; oid_index < sizeof (discovery_oids) /
                       sizeof (discovery_oids[0]); oid_index++)
    {
      parsed_oid_len = MAX_OID_LEN;
      if (read_objid (discovery_oids[oid_index], parsed_oid,
                      &parsed_oid_len))
        snmp_add_null_var (request, parsed_oid, parsed_oid_len);
    }
  if (!snmp_send (ss, request))
    {
      snmp_free_pdu (request);
      PDBG (pixma_dbg (2, "SNMP broadcast send failed\n"));
    }
  else
    {
      struct timeval timeout = { 0, 125000 };
      struct timeval end;
      fd_set fdset;
      int fds, block;
      gettimeofday (&end, NULL);
      end.tv_sec += CANON_SNMP_DISCOVERY_TIMEOUT_MS / 1000;
      end.tv_usec += (CANON_SNMP_DISCOVERY_TIMEOUT_MS % 1000) * 1000;
      if (end.tv_usec >= 1000000)
        {
          end.tv_sec++;
          end.tv_usec -= 1000000;
        }
      do
        {
          FD_ZERO (&fdset);
          timeout.tv_sec = 0;
          timeout.tv_usec = 125000;
          fds = 0;
          block = 0;
          snmp_select_info (&fds, &fdset, &timeout, &block);
          if (select (fds, &fdset, NULL, NULL, &timeout) > 0)
            snmp_read (&fdset);
          else
            snmp_timeout ();
          gettimeofday (&timeout, NULL);
        }
      while (timeout.tv_sec < end.tv_sec ||
             (timeout.tv_sec == end.tv_sec && timeout.tv_usec < end.tv_usec));
    }
  snmp_close (ss);
  SOCK_CLEANUP;
}
#endif

static void
clear_scanner_list (void)
{
  scanner_info_t *si = first_scanner;
  while (si)
    {
      scanner_info_t *temp = si;
      free (si->devname);
      si = si->next;
      free (temp);
    }
  nscanners = 0;
  first_scanner = NULL;
}

static SANE_Status
get_descriptor (SANE_Int dn, SANE_Int type, SANE_Int descidx,
		SANE_Int index, SANE_Int length, SANE_Byte * data)
{
  return sanei_usb_control_msg (dn, 0x80, USB_REQ_GET_DESCRIPTOR,
				((type & 0xff) << 8) | (descidx & 0xff),
				index, length, data);
}

static SANE_Status
get_string_descriptor (SANE_Int dn, SANE_Int index, SANE_Int lang,
		       SANE_Int length, SANE_Byte * data)
{
  return get_descriptor (dn, USB_DT_STRING, index, lang, length, data);
}

static void
u16tohex (uint16_t x, char *str)
{
  static const char hdigit[16] =
    { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D',
    'E', 'F'
    };
  str[0] = hdigit[(x >> 12) & 0xf];
  str[1] = hdigit[(x >> 8) & 0xf];
  str[2] = hdigit[(x >> 4) & 0xf];
  str[3] = hdigit[x & 0xf];
  str[4] = '\0';
}

static void
read_serial_number (scanner_info_t * si)
{
  uint8_t unicode[2 * (PIXMA_MAX_ID_LEN - 9) + 2];	// 9 = size of VID + PID + "_"
  uint8_t ddesc[18];
  int iSerialNumber;
  SANE_Int usb;
  char *serial = si->serial;

  u16tohex (si->cfg->vid, serial);
  u16tohex (si->cfg->pid, serial + 4);

  if (SANE_STATUS_GOOD != sanei_usb_open (si->devname, &usb))
    return;
  if (get_descriptor (usb, USB_DT_DEVICE, 0, 0, 18, ddesc)
      != SANE_STATUS_GOOD)
    goto done;
  iSerialNumber = ddesc[16];
  if (iSerialNumber != 0)
    {
      int i, len;
      SANE_Status status;

      /*int iSerialNumber = ddesc[16];*/
      /* Read the first language code. Assumed that there is at least one. */
      if (get_string_descriptor (usb, 0, 0, 4, unicode) != SANE_STATUS_GOOD)
        goto done;
      /* Read the serial number string. */
      status = get_string_descriptor (usb, iSerialNumber,
                                      unicode[3] * 256 + unicode[2],
                                      sizeof (unicode), unicode);
      if (status != SANE_STATUS_GOOD)
        goto done;
      /* Assumed charset: Latin1 */
      len = unicode[0];
      if (len > (int) sizeof (unicode))
        {
          len = sizeof (unicode);
          PDBG (pixma_dbg (1, "WARNING:Truncated serial number\n"));
        }
            serial[8] = '_';
            for (i = 2; i < len; i += 2)
        {
          serial[9 + i / 2 - 1] = unicode[i];
        }
      serial[9 + i / 2 - 1] = '\0';
    }
  else
    {
      PDBG (pixma_dbg (1, "WARNING:No serial number\n"));
    }
done:
  sanei_usb_close (usb);
}

static int
map_error (SANE_Status ss)
{
  switch (ss)
    {
    case SANE_STATUS_GOOD:
      return 0;
    case SANE_STATUS_UNSUPPORTED:
      return PIXMA_ENODEV;
    case SANE_STATUS_DEVICE_BUSY:
      return PIXMA_EBUSY;
    case SANE_STATUS_INVAL:
      return PIXMA_EINVAL;
    case SANE_STATUS_IO_ERROR:
      return PIXMA_EIO;
    case SANE_STATUS_NO_MEM:
      return PIXMA_ENOMEM;
    case SANE_STATUS_ACCESS_DENIED:
      return PIXMA_EACCES;
    case SANE_STATUS_CANCELLED:
      return PIXMA_ECANCELED;
    case SANE_STATUS_JAMMED:
       return PIXMA_EPAPER_JAMMED;
    case SANE_STATUS_COVER_OPEN:
       return PIXMA_ECOVER_OPEN;
    case SANE_STATUS_NO_DOCS:
       return PIXMA_ENO_PAPER;
    case SANE_STATUS_EOF:
       return PIXMA_EOF;
#ifdef SANE_STATUS_HW_LOCKED
    case SANE_STATUS_HW_LOCKED:       /* unused by pixma */
#endif
#ifdef SANE_STATUS_WARMING_UP
    case SANE_STATUS_WARMING_UP:      /* unused by pixma */
#endif
      break;
    }
  PDBG (pixma_dbg (1, "BUG:Unmapped SANE Status code %d\n", ss));
  return PIXMA_EIO;		/* should not happen */
}


int
pixma_io_init (void)
{
  sanei_usb_init ();
  sanei_bjnp_init();
#if HAVE_LIBCURL
  curl_global_init (CURL_GLOBAL_DEFAULT);
#endif
  nscanners = 0;
  return 0;
}

void
pixma_io_cleanup (void)
{
  while (first_io)
    pixma_disconnect (first_io);
  clear_scanner_list ();
  sanei_bjnp_cleanup ();
#if HAVE_LIBCURL
  curl_global_cleanup ();
#endif
}

unsigned
pixma_collect_devices (const char **conf_devices,
                       const struct pixma_config_t *const pixma_devices[], SANE_Bool local_only)
{
  unsigned i, j;
  struct scanner_info_t *si;
  const struct pixma_config_t *cfg;

  clear_scanner_list ();
  j = 0;
  for (i = 0; pixma_devices[i]; i++)
    {
      for (cfg = pixma_devices[i]; cfg->name; cfg++)
        {
          sanei_usb_find_devices (cfg->vid, cfg->pid, attach);
          si = first_scanner;
          while (j < nscanners)
            {
              PDBG (pixma_dbg (3, "pixma_collect_devices() found %s at %s\n",
                   cfg->name, si->devname));
              si->cfg = cfg;
              read_serial_number (si);
              si = si->next;
              j++;
            }
        }
    }
  if (! local_only)
    {
      sanei_bjnp_find_devices(conf_devices, attach_bjnp, pixma_devices);
#if HAVE_LIBSNMP
      canon_snmp_discover (pixma_devices);
#endif
    }

  si = first_scanner;
  while (j < nscanners)
    {
      PDBG (pixma_dbg (3, "pixma_collect_devices() found %s at %s\n",
               si->cfg->name, si->devname));
      si = si->next;
      j++;

    }
  return nscanners;
}

const pixma_config_t *
pixma_get_device_config (unsigned devnr)
{
  const scanner_info_t *si = get_scanner_info (devnr);
  return (si) ? si->cfg : NULL;
}

const char *
pixma_get_device_id (unsigned devnr)
{
  const scanner_info_t *si = get_scanner_info (devnr);
  return (si) ? si->serial : NULL;
}

int
pixma_connect (unsigned devnr, pixma_io_t ** handle)
{
  pixma_io_t *io;
  SANE_Int dev;
  const scanner_info_t *si;
  int error;

  *handle = NULL;
  si = get_scanner_info (devnr);
  if (!si)
    return PIXMA_EINVAL;
  if (si-> interface == INT_BJNP)
    error = map_error (sanei_bjnp_open (si->devname, &dev));
  else if (si->interface == INT_CANON_HTTP)
    {
#if HAVE_LIBCURL
      dev = -1;
      error = 0;
#else
      error = PIXMA_ENOTSUP;
#endif
    }
  else
    error = map_error (sanei_usb_open (si->devname, &dev));

  if (error < 0)
    return error;
  io = (pixma_io_t *) calloc (1, sizeof (*io));
  if (!io)
    {
      if (si -> interface == INT_BJNP)
        sanei_bjnp_close (dev);
      else if (si->interface != INT_CANON_HTTP)
        sanei_usb_close (dev);
      return PIXMA_ENOMEM;
    }
  io->dev = dev;
  io->interface = si->interface;
#if HAVE_LIBCURL
  if (io->interface == INT_CANON_HTTP)
    {
      const char *host = si->devname + strlen ("canonhttp://");
      if (asprintf (&io->http_url,
                    "http://%s/canon/ij/command2/port3", host) < 0)
        {
          free (io);
          return PIXMA_ENOMEM;
        }
    }
#endif
  io->next = first_io;
  first_io = io;
  *handle = io;
  return 0;
}


void
pixma_disconnect (pixma_io_t * io)
{
  pixma_io_t **p;

  if (!io)
    return;
  for (p = &first_io; *p && *p != io; p = &((*p)->next))
    {
    }
  PASSERT (*p);
  if (!(*p))
    return;
  if (io-> interface == INT_BJNP)
    sanei_bjnp_close (io->dev);
  else if (io->interface != INT_CANON_HTTP)
    sanei_usb_close (io->dev);
#if HAVE_LIBCURL
  if (io->http_curl != NULL)
    curl_easy_cleanup (io->http_curl);
  curl_slist_free_all (io->http_headers);
  free (io->http_url);
  free (io->http_data);
#endif
  *p = io->next;
  free (io);
}

int pixma_activate (pixma_io_t * io)
{
  int error;
  if (io->interface == INT_BJNP)
    {
      error = map_error(sanei_bjnp_activate (io->dev));
    }
  else if (io->interface == INT_CANON_HTTP)
    error = 0;
  else
    /* noop for USB interface */
    error = 0;
  return error;
}

int pixma_deactivate (pixma_io_t * io)
{
  int error;
  if (io->interface == INT_BJNP)
    {
      error = map_error(sanei_bjnp_deactivate (io->dev));
    }
  else if (io->interface == INT_CANON_HTTP)
    error = 0;
  else
    /* noop for USB interface */
    error = 0;
  return error;

}

int
pixma_reset_device (pixma_io_t * io)
{
  UNUSED (io);
  return PIXMA_ENOTSUP;
}

int
pixma_write (pixma_io_t * io, const void *cmd, unsigned len)
{
  size_t count = len;
  int error;

  if (io->interface == INT_BJNP)
    {
    sanei_bjnp_set_timeout (io->dev, PIXMA_BULKOUT_TIMEOUT);
    error = map_error (sanei_bjnp_write_bulk (io->dev, cmd, &count));
    }
  else if (io->interface == INT_CANON_HTTP)
#if HAVE_LIBCURL
    error = canon_http_request (io, cmd, len, 0);
#else
    error = PIXMA_ENOTSUP;
#endif
  else
    {
#ifdef HAVE_SANEI_USB_SET_TIMEOUT
    sanei_usb_set_timeout (PIXMA_BULKOUT_TIMEOUT);
#endif
    error = map_error (sanei_usb_write_bulk (io->dev, cmd, &count));
    }
  if (error == PIXMA_EIO)
    error = PIXMA_ETIMEDOUT;	/* FIXME: SANE doesn't have ETIMEDOUT!! */
  if (count != len)
    {
      PDBG (pixma_dbg (1, "WARNING:pixma_write(): count(%u) != len(%u)\n",
		       (unsigned) count, len));
      error = PIXMA_EIO;
    }
  if (error >= 0)
    error = count;
  PDBG (pixma_dump (10, "OUT ", cmd, error, len, 128));
  return error;
}

int
pixma_read (pixma_io_t * io, void *buf, unsigned size)
{
  size_t count = size;
  int error;

  if (io-> interface == INT_BJNP)
    {
    sanei_bjnp_set_timeout (io->dev, PIXMA_BULKIN_TIMEOUT);
    error = map_error (sanei_bjnp_read_bulk (io->dev, buf, &count));
    }
  else if (io->interface == INT_CANON_HTTP)
    {
#if HAVE_LIBCURL
      if (io->http_pos == io->http_len &&
          canon_http_request (io, NULL, 0, 1) < 0)
        error = PIXMA_EIO;
      else
        {
          count = io->http_len - io->http_pos;
          if (count > size)
            count = size;
          memcpy (buf, io->http_data + io->http_pos, count);
          io->http_pos += count;
          error = 0;
        }
#else
      error = PIXMA_ENOTSUP;
#endif
    }
  else
    {
#ifdef HAVE_SANEI_USB_SET_TIMEOUT
      sanei_usb_set_timeout (PIXMA_BULKIN_TIMEOUT);
#endif
      error = map_error (sanei_usb_read_bulk (io->dev, buf, &count));
    }

  if (error == PIXMA_EIO)
    error = PIXMA_ETIMEDOUT;	/* FIXME: SANE doesn't have ETIMEDOUT!! */
  if (error >= 0)
    error = count;
  PDBG (pixma_dump (10, "IN  ", buf, error, -1, 128));
  return error;
}

int
pixma_wait_interrupt (pixma_io_t * io, void *buf, unsigned size, int timeout)
{
  size_t count = size;
  int error;

  /* FIXME: What is the meaning of "timeout" in sanei_usb? */
  if (timeout < 0)
    timeout = INT_MAX;
  else if (timeout < 100)
    timeout = 100;
  if (io-> interface == INT_BJNP)
    {
      sanei_bjnp_set_timeout (io->dev, timeout);
      error = map_error (sanei_bjnp_read_int (io->dev, buf, &count));
    }
  else if (io->interface == INT_CANON_HTTP)
    /* Canon's HTTP transport has no separate interrupt endpoint. */
    error = PIXMA_ETIMEDOUT;
  else
    {
#ifdef HAVE_SANEI_USB_SET_TIMEOUT
      sanei_usb_set_timeout (timeout);
#endif
      error = map_error (sanei_usb_read_int (io->dev, buf, &count));
    }
  if (error == PIXMA_EIO ||
      (io->interface == INT_BJNP && error == PIXMA_EOF))     /* EOF is a bjnp timeout error! */
    error = PIXMA_ETIMEDOUT;	/* FIXME: SANE doesn't have ETIMEDOUT!! */
  if (error == 0)
    error = count;
  if (error != PIXMA_ETIMEDOUT)
    PDBG (pixma_dump (10, "INTR", buf, error, -1, -1));
  return error;
}

int
pixma_set_interrupt_mode (pixma_io_t * s, int background)
{
  UNUSED (s);
  return (background) ? PIXMA_ENOTSUP : 0;
}
