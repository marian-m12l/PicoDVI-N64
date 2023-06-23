#pragma once

#define HTTP_GET "GET"
#define HTTP_POST "POST"

#define OTA_PATH "/ota"
#define BOOT_PATH "/boot"

#define HTTP_400 "<html><body>Bad Request</body></html>"
#define HTTP_404 "<html><body>Not Found</body></html>"

#define HTTP_RESPONSE_REDIRECT "HTTP/1.1 302 Redirect\nLocation: http://%s" OTA_PATH "\n\n"

extern const char* HTTP_RESPONSE_HEADERS;
extern const char* LOGO_SVG;
extern const char* OTA_BODY;
extern const char* OTA_BODY_SUCCESS;
extern const char* OTA_BODY_FAILURE;
