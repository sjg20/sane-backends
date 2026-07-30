/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 SANE Project

   This file is part of the SANE package and is distributed under the
   terms of the GNU General Public License version 3 or later.

   Unit tests for code paths which do not require a physical scanner. */

#define DEBUG_DECLARE_ONLY
#include "backend/escl/escl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
int sanei_debug_escl;

void
sanei_debug_escl_call(int level, const char *message, ...)
{
    (void)level;
    (void)message;
}

SANE_String_Const
sane_strstatus(SANE_Status status)
{
    (void)status;
    return "";
}

static void
expect_status(const char *name, SANE_Status actual, SANE_Status expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got %d, expected %d\n", name, actual, expected);
        failures++;
    }
}

static void
test_http_status(void)
{
    expect_status("HTTP 200", escl_http_status(200), SANE_STATUS_GOOD);
    expect_status("HTTP 204", escl_http_status(204), SANE_STATUS_GOOD);
    expect_status("HTTP 401", escl_http_status(401), SANE_STATUS_ACCESS_DENIED);
    expect_status("HTTP 409", escl_http_status(409), SANE_STATUS_NO_DOCS);
    expect_status("HTTP 503", escl_http_status(503), SANE_STATUS_DEVICE_BUSY);
    expect_status("HTTP 500", escl_http_status(500), SANE_STATUS_IO_ERROR);
}

static void
test_disable_https_config_option(void)
{
    SANE_Bool disabled = SANE_FALSE;

    if (!escl_parse_disable_https("option disable-https yes", &disabled) ||
        disabled != SANE_TRUE) {
        fprintf(stderr, "disable-https yes was not parsed\n");
        failures++;
    }

    if (!escl_parse_disable_https("  option disable-https no", &disabled) ||
        disabled != SANE_FALSE) {
        fprintf(stderr, "disable-https no was not parsed\n");
        failures++;
    }

    disabled = SANE_TRUE;
    if (escl_parse_disable_https("device http://127.0.0.1:8080", &disabled) ||
        disabled != SANE_TRUE) {
        fprintf(stderr, "unrelated configuration changed disable-https\n");
        failures++;
    }
}

static void
send_http_response(int fd, int status, const char *body)
{
    char response[256];
    int length = snprintf(response, sizeof(response),
                          "HTTP/1.1 %d Test\r\nContent-Length: %zu\r\n"
                          "Connection: close\r\n\r\n%s",
                          status, strlen(body), body);
    send(fd, response, (size_t)length, 0);
}

static void
test_scan_retry_replaces_response_body(void)
{
    const char busy_body[] = "busy response that is longer than the image";
    const char image_body[] = "OK";
    ESCL_Device device = { 0 };
    capabilities_t scanner = { 0 };
    struct sockaddr_in address = { 0 };
    char address_text[] = "127.0.0.1";
    char contents[sizeof(busy_body)] = { 0 };
    int server_fd, client_fd;
    socklen_t address_size = sizeof(address);
    pid_t child;
    size_t bytes_read;
    SANE_Status status;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "could not create HTTP test socket\n");
        failures++;
        return;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server_fd, 2) < 0 ||
        getsockname(server_fd, (struct sockaddr *)&address, &address_size) < 0) {
        fprintf(stderr, "could not configure HTTP test socket\n");
        close(server_fd);
        failures++;
        return;
    }

    child = fork();
    if (child == 0) {
        char request[1024];
        for (int i = 0; i < 2; i++) {
            client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0)
                _exit(EXIT_FAILURE);
            (void)recv(client_fd, request, sizeof(request), 0);
            send_http_response(client_fd, i == 0 ? 503 : 200,
                               i == 0 ? busy_body : image_body);
            close(client_fd);
        }
        close(server_fd);
        _exit(EXIT_SUCCESS);
    }
    if (child < 0) {
        fprintf(stderr, "could not fork HTTP test server\n");
        close(server_fd);
        failures++;
        return;
    }
    close(server_fd);

    device.ip_address = address_text;
    device.port_nb = ntohs(address.sin_port);
    status = escl_scan(&scanner, &device, "ScanJobs", "/1");
    if (status != SANE_STATUS_GOOD || !scanner.tmp) {
        fprintf(stderr, "HTTP retry did not return a scan image\n");
        failures++;
    } else {
        fseek(scanner.tmp, 0, SEEK_SET);
        bytes_read = fread(contents, 1, sizeof(contents), scanner.tmp);
        if (bytes_read != sizeof(image_body) - 1 ||
            memcmp(contents, image_body, sizeof(image_body) - 1) != 0) {
            fprintf(stderr, "HTTP retry retained bytes from the busy response\n");
            failures++;
        }
        fclose(scanner.tmp);
        scanner.tmp = NULL;
    }
    waitpid(child, NULL, 0);
}

static void
test_scan_file_reset(void)
{
    capabilities_t scanner = { 0 };
    const char busy_body[] = "scanner is busy and returned a long response";
    const char image[] = "image";
    char contents[sizeof(busy_body)] = { 0 };
    size_t bytes_read;

    scanner.tmp = tmpfile();
    if (!scanner.tmp) {
        fprintf(stderr, "could not create scan temporary file\n");
        failures++;
        return;
    }
    fwrite(busy_body, 1, sizeof(busy_body) - 1, scanner.tmp);
    scanner.real_read = sizeof(busy_body) - 1;

    expect_status("scan file reset", escl_reset_scan_file(&scanner),
                  SANE_STATUS_GOOD);
    if (!scanner.tmp)
        return;

    fwrite(image, 1, sizeof(image) - 1, scanner.tmp);
    fseek(scanner.tmp, 0, SEEK_SET);
    bytes_read = fread(contents, 1, sizeof(contents), scanner.tmp);
    if (bytes_read != sizeof(image) - 1 ||
        memcmp(contents, image, sizeof(image) - 1) != 0) {
        fprintf(stderr, "scan file reset retained data from the busy response\n");
        failures++;
    }
    fclose(scanner.tmp);
}

static void
test_crop_passthrough(void)
{
    capabilities_t scanner = { 0 };
    unsigned char *surface = malloc(12);
    unsigned char *result;
    int width = 0;
    int height = 0;

    memset(surface, 7, 12);
    scanner.caps[PLATEN].width = 600;
    scanner.caps[PLATEN].height = 600;
    scanner.caps[PLATEN].default_resolution = 300;
    result = escl_crop_surface(&scanner, surface, 2, 2, 3, &width, &height);
    if (result != surface || width != 2 || height != 2 ||
        scanner.img_size != 12) {
        fprintf(stderr, "crop passthrough returned unexpected image\n");
        failures++;
    }
    free(result);
}

#if defined HAVE_LIBPNG
static void
test_png_grayscale_and_orientation(void)
{
    static const unsigned char png_data[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x00, 0x00, 0x00, 0x00, 0x57, 0xdd, 0x52,
        0xf8, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9c, 0x63, 0x10, 0x50, 0x60, 0x30,
        0x70, 0x00, 0x00, 0x01, 0x76, 0x00, 0xa1, 0xec,
        0x30, 0x8a, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    const unsigned char expected[] = {
        0x10, 0x10, 0x10, 0x20, 0x20, 0x20,
        0x30, 0x30, 0x30, 0x40, 0x40, 0x40
    };
    capabilities_t scanner = { 0 };
    int width = 0, height = 0, bps = 0;

    scanner.tmp = tmpfile();
    if (!scanner.tmp || fwrite(png_data, 1, sizeof(png_data), scanner.tmp) !=
                              sizeof(png_data)) {
        fprintf(stderr, "could not prepare PNG test image\n");
        if (scanner.tmp)
            fclose(scanner.tmp);
        failures++;
        return;
    }
    fseek(scanner.tmp, 0, SEEK_SET);
    if (get_PNG_data(&scanner, &width, &height, &bps) != SANE_STATUS_GOOD ||
        width != 2 || height != 2 || bps != 3 || scanner.img_size != 12 ||
        memcmp(scanner.img_data, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "PNG grayscale decoding returned unexpected pixels\n");
        failures++;
    }
    free(scanner.img_data);
    scanner.img_data = NULL;
}
#endif

static void
test_crop_full_surface_fallback(void)
{
    capabilities_t scanner = { 0 };
    unsigned char expected[] = {
        18, 19, 20, 21,
        26, 27, 28, 29,
        34, 35, 36, 37,
        42, 43, 44, 45
    };
    unsigned char *surface = malloc(64);
    unsigned char *result;
    int width = 0;
    int height = 0;

    for (int i = 0; i < 64; i++)
        surface[i] = (unsigned char)i;
    scanner.caps[PLATEN].width = 600;
    scanner.caps[PLATEN].height = 600;
    scanner.caps[PLATEN].pos_x = 300;
    scanner.caps[PLATEN].pos_y = 300;
    scanner.caps[PLATEN].MaxWidth = 1200;
    scanner.caps[PLATEN].MaxHeight = 1200;
    scanner.caps[PLATEN].default_resolution = 1;

    result = escl_crop_surface(&scanner, surface, 8, 8, 1, &width, &height);
    if (!result || width != 4 || height != 4 || scanner.img_size != 16 ||
        memcmp(result, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "fallback crop returned unexpected image\n");
        failures++;
    }
    free(result);
}

int
main(void)
{
    test_http_status();
    test_disable_https_config_option();
    test_scan_retry_replaces_response_body();
    test_scan_file_reset();
    test_crop_passthrough();
#if defined HAVE_LIBPNG
    test_png_grayscale_and_orientation();
#endif
    test_crop_full_surface_fallback();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
