#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/flash.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"

#include <stdio.h>
#include <stdarg.h>

#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "dhcpserver.h"
#include "dnsserver.h"

#include "hardware/resets.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/systick.h"

#include "pico/binary_info/defs.h"
#include "pico/binary_info/structure.h"


/*************************************/
/**********       DVI       **********/
/*************************************/

// DVDD 1.2V (1.1V seems ok too)
#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

// RGB111 bitplaned framebuffer
#define PLANE_SIZE_BYTES (FRAME_WIDTH * FRAME_HEIGHT / 8)
#define RED 0x4
#define GREEN 0x2
#define BLUE 0x1
#define BLACK 0x0
#define WHITE 0x7
uint8_t framebuf[3 * PLANE_SIZE_BYTES];

struct dvi_inst dvi0;


/*************************************/
/**********    TERMINAL     **********/
/*************************************/

#include "font_8x8.h"
#define FONT_CHAR_WIDTH 8
#define FONT_CHAR_HEIGHT 8
#define FONT_N_CHARS 95
#define FONT_FIRST_ASCII 32

const uint FIRST_LINE = 40;
const uint LAST_LINE = 430;
const uint FOOTER_LINE = 450;
const uint LABEL_LEFT = 18;
uint16_t line = FIRST_LINE;


/*************************************/
/**********   TCP / HTTP    **********/
/*************************************/

#define TCP_PORT 80
#define DEBUG_printf printf
#define POLL_TIME_S 5
#define HTTP_GET "GET"
#define HTTP_POST "POST"
#define OTA_PATH "/ota"
#define BOOT_PATH "/boot"
#define HTTP_RESPONSE_HEADERS "HTTP/1.1 %s\nContent-Length: %d\nContent-Type: text/html; charset=utf-8\nConnection: close\n\n"
#define OTA_BODY "<html><body><h1>Over-the-Air Firmware Upgrade.</h1><p>Installed: %s</p><p>Build date: %s</p><form method=\"POST\" action=\"%s\" enctype=\"multipart/form-data\"><input type=\"file\" id=\"firmware\" name=\"firmware\"/><input type=\"submit\" value=\"Upgrade\" /></form><form method=\"POST\" action=\"%s\"><input type=\"submit\" value=\"Boot stage 2\" /></form></body></html>"
#define OTA_BODY_SUCCESS "<html><body><h1>Over-the-Air Firmware Upgrade.</h1><h2>Upgrade successful!</h2><p><a href=\"" OTA_PATH "\">Back</a></p></body></html>"
#define OTA_BODY_FAILURE "<html><body><h1>Over-the-Air Firmware Upgrade.</h1><h2>Upgrade Failed!</h2><p><a href=\"" OTA_PATH "\">Back</a></p></body></html>"
#define HTTP_400 "<html><body>Bad Request</body></html>"
#define HTTP_404 "<html><body>Not Found</body></html>"
#define HTTP_RESPONSE_REDIRECT "HTTP/1.1 302 Redirect\nLocation: http://%s" OTA_PATH "\n\n"

typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    bool complete;
    ip_addr_t gw;
} TCP_SERVER_T;

typedef struct TCP_CONNECT_STATE_T_ {
    struct tcp_pcb *pcb;
    int sent_len;
    char headers[128];
    char result[512];
    int header_len;
    int result_len;
    ip_addr_t *gw;
} TCP_CONNECT_STATE_T;

TCP_SERVER_T *state;
dhcp_server_t dhcp_server;
dns_server_t dns_server;


/*************************************/
/********** UPLOAD / FLASH  **********/
/*************************************/

static bool fw_recv = false;
static char fw_remote_ip[16];
static uint16_t fw_remote_port = 0;
static uint32_t fw_total_length = 0;
static uint16_t fw_total_sectors = 0;
static uint32_t fw_total_received = 0;
static uint32_t fw_packets_received = 0;
static bool fw_flash_ok = true;
static uint16_t fw_flash_sector = 0;
static uint16_t fw_buffer_filled = 0;
static uint8_t fw_buffer[4096];

#define CRLF "\r\n"
#define HTTP_HDR_CONTENT_LEN                "Content-Length: "
#define HTTP_HDR_CONTENT_LEN_LEN            16
#define HTTP_HDR_CONTENT_LEN_DIGIT_MAX_LEN  10

// Firmware is flashed at a 1MB offset (0x100000)
#define FLASH_TARGET_OFFSET (1024 * 1024)
const uint8_t *flash_target_contents = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET);


/*************************************/
/**********   BINARY INFO   **********/
/*************************************/

typedef struct firmware_info_t {
    bool exists;
    char* prog_name;
    char* prog_version;
    char* prog_date;
    char* sdk_version;
    char* pico_board;
} firmware_info_t;

firmware_info_t fw_info_slot1;



/*************************************/
/**********    TERMINAL     **********/
/*************************************/

static inline void putpixel(uint x, uint y, uint rgb) {
	uint8_t mask = 1u << (x % 8);
	for (uint component = 0; component < 3; ++component) {
		uint idx = (x / 8) + y * FRAME_WIDTH / 8 + component * PLANE_SIZE_BYTES;
		if (rgb & (1u << component))
			framebuf[idx] |= mask;
		else
			framebuf[idx] &= ~mask;
	}
}

void fillrect(uint x0, uint y0, uint x1, uint y1, uint rgb) {
	for (uint x = x0; x <= x1; ++x)
		for (uint y = y0; y <= y1; ++y)
			putpixel(x, y, rgb);
}

void puttext(uint x0, uint y0, uint bgcol, uint fgcol, const char *text) {
	for (int y = y0; y < y0 + 8; ++y) {
		uint xbase = x0;
		const char *ptr = text;
		char c;
		while ((c = *ptr++)) {
			uint8_t font_bits = font_8x8[(c - FONT_FIRST_ASCII) + (y - y0) * FONT_N_CHARS];
			for (int i = 0; i < 8; ++i)
				putpixel(xbase + i, y, font_bits & (1u << i) ? fgcol : bgcol);
			xbase += 8;
		}
	}
}

void puttextf_valist(uint x0, uint y0, uint bgcol, uint fgcol, const char *fmt, va_list args) {
	char buf[77];
	vsnprintf(buf, 77, fmt, args);
	puttext(x0, y0, bgcol, fgcol, buf);
}

void puttextf(uint x0, uint y0, uint bgcol, uint fgcol, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	puttextf_valist(x0, y0, bgcol, fgcol, fmt, args);
	va_end(args);
}

void ffw() {
    line += 10;
    if (line > LAST_LINE) {
        line = FIRST_LINE;
    }
}

void rwd() {
    line -= 10;
    if (line < FIRST_LINE) {
        line = LAST_LINE;
    }
}

void printlinef(uint bgcol, uint fgcol, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	puttextf_valist(LABEL_LEFT, line, bgcol, fgcol, fmt, args);
	puttextf_valist(LABEL_LEFT, line+10, WHITE, WHITE, "                                                                            ", args);
	ffw();
	va_end(args);
}

void printfooterf(uint bgcol, uint fgcol, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	puttextf_valist(LABEL_LEFT, FOOTER_LINE, bgcol, fgcol, fmt, args);
	va_end(args);
}

void print_progress(bool flashing, uint8_t flashed, uint8_t total) {
    /*printfooterf(WHITE, BLACK,
        "PROGRAMMING [%.*s%s%.*s]",
        flashed, "============================================================",
        flashing ? ">" : "",
        total - flashed - (flashing ? 1 : 0), "------------------------------------------------------------");*/
    printfooterf(WHITE, BLACK, "PROGRAMMING [ %3d / %3d ]", flashed, total);
}



/*************************************/
/**********       DVI       **********/
/*************************************/

void core1_main() {
	dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
	dvi_start(&dvi0);
	while (true) {
		for (uint y = 0; y < FRAME_HEIGHT; ++y) {
			uint32_t *tmdsbuf;
			queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
			for (uint component = 0; component < 3; ++component) {
				tmds_encode_1bpp(
					(const uint32_t*)&framebuf[y * FRAME_WIDTH / 8 + component * PLANE_SIZE_BYTES],
					tmdsbuf + component * FRAME_WIDTH / DVI_SYMBOLS_PER_WORD,
					FRAME_WIDTH
				);
			}
			queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
		}
	}
}



/*************************************/
/********** UPLOAD / FLASH  **********/
/*************************************/

bool program_flash_sector() {
    uint32_t offset = FLASH_TARGET_OFFSET + fw_flash_sector*0x1000;

    printlinef(WHITE, RED, "ERASE sector %d @ 0x%08x", fw_flash_sector, offset);
	flash_range_erase(offset, FLASH_SECTOR_SIZE);
    
    printlinef(WHITE, BLACK, "PROGRAM sector %d @ 0x%08x", fw_flash_sector, offset);
    flash_range_program(offset, fw_buffer, sizeof(fw_buffer));  // TODO only flash up to fw_buffer_filled ??

    bool mismatch = false;
    const uint8_t *programmed_content = (const uint8_t *) (XIP_BASE + offset);
    for (int i = 0; i < sizeof(fw_buffer); ++i) {
        if (fw_buffer[i] != programmed_content[i])
            mismatch = true;
    }
    if (mismatch)
        printlinef(WHITE, RED, "PROGRAM of sector %d @ 0x%08x FAILED", fw_flash_sector, offset);
    else
        printlinef(WHITE, GREEN, "PROGRAM of sector %d @ 0x%08x SUCCEEDED", fw_flash_sector, offset);
	
	return mismatch;
}



/*************************************/
/**********   TCP / HTTP    **********/
/*************************************/

static err_t tcp_close_client_connection(TCP_CONNECT_STATE_T *con_state, struct tcp_pcb *client_pcb, err_t close_err) {
    if (client_pcb) {
        assert(con_state && con_state->pcb == client_pcb);
        tcp_arg(client_pcb, NULL);
        tcp_poll(client_pcb, NULL, 0);
        tcp_sent(client_pcb, NULL);
        tcp_recv(client_pcb, NULL);
        tcp_err(client_pcb, NULL);
        err_t err = tcp_close(client_pcb);
        if (err != ERR_OK) {
            DEBUG_printf("close failed %d, calling abort\n", err);
            tcp_abort(client_pcb);
            close_err = ERR_ABRT;
        }
        if (con_state) {
            free(con_state);
        }
    }
    return close_err;
}

static void tcp_server_close(TCP_SERVER_T *state) {
    if (state->server_pcb) {
        tcp_arg(state->server_pcb, NULL);
        tcp_close(state->server_pcb);
        state->server_pcb = NULL;
    }
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    DEBUG_printf("tcp_server_sent %u\n", len);
    con_state->sent_len += len;
    if (con_state->sent_len >= con_state->header_len + con_state->result_len) {
        DEBUG_printf("all done\n");
        return tcp_close_client_connection(con_state, pcb, ERR_OK);
    }
    return ERR_OK;
}

void send_response(TCP_CONNECT_STATE_T* con_state, struct tcp_pcb* pcb) {
    // Check we had enough buffer space
    if (con_state->result_len > sizeof(con_state->result) - 1) {
        DEBUG_printf("Too much result data %d\n", con_state->result_len);
        return tcp_close_client_connection(con_state, pcb, ERR_CLSD);
    }
    if (con_state->header_len > sizeof(con_state->headers) - 1) {
        DEBUG_printf("Too much header data %d\n", con_state->header_len);
        return tcp_close_client_connection(con_state, pcb, ERR_CLSD);
    }

    // Send the headers to the client
    con_state->sent_len = 0;
    err_t err = tcp_write(pcb, con_state->headers, con_state->header_len, 0);
    if (err != ERR_OK) {
        DEBUG_printf("failed to write header data %d\n", err);
        return tcp_close_client_connection(con_state, pcb, err);
    }

    // Send the body to the client
    if (con_state->result_len) {
        err = tcp_write(pcb, con_state->result, con_state->result_len, 0);
        if (err != ERR_OK) {
            DEBUG_printf("failed to write result data %d\n", err);
            return tcp_close_client_connection(con_state, pcb, err);
        }
    }
}

err_t tcp_server_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    if (!p) {
        DEBUG_printf("connection closed\n");
        return tcp_close_client_connection(con_state, pcb, ERR_OK);
    }
    assert(con_state && con_state->pcb == pcb);
    if (p->tot_len > 0) {
        DEBUG_printf("tcp_server_recv %d err %d\n", p->tot_len, err);
		//printlinef(WHITE, BLACK, "TCP recv %d err %d.", p->tot_len, err);
        // Copy the request into the buffer
        pbuf_copy_partial(p, con_state->headers, p->tot_len > sizeof(con_state->headers) - 1 ? sizeof(con_state->headers) - 1 : p->tot_len, 0);

        // Handle GET and POST requests
		bool isGet = strncmp(HTTP_GET, con_state->headers, sizeof(HTTP_GET) - 1) == 0;
		bool isPost = strncmp(HTTP_POST, con_state->headers, sizeof(HTTP_POST) - 1) == 0;
        bool isFirmwareUploadClient = (strncmp(ipaddr_ntoa(&pcb->remote_ip), fw_remote_ip, sizeof(fw_remote_ip) - 1) == 0) && pcb->remote_port == fw_remote_port;
        if (isGet) {
            //printlinef(WHITE, BLACK, "GET request.");
            char *request = con_state->headers + sizeof(HTTP_GET); // + space

            if (strncmp(request, OTA_PATH, sizeof(OTA_PATH) - 1) == 0) {
                // Main page
                char* progname = (fw_info_slot1.exists && fw_info_slot1.prog_name != 0) ? fw_info_slot1.prog_name : "";
                char* progdate = (fw_info_slot1.exists && fw_info_slot1.prog_date != 0) ? fw_info_slot1.prog_date : "";
                con_state->result_len = snprintf(con_state->result, sizeof(con_state->result), OTA_BODY, progname, progdate, OTA_PATH, BOOT_PATH);
                con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_HEADERS, "200 OK", con_state->result_len);
                send_response(con_state, pcb);
            } else {
				// Send redirect
				con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_REDIRECT, ipaddr_ntoa(con_state->gw));
                send_response(con_state, pcb);
				DEBUG_printf("Sending redirect %s", con_state->headers);
			}
        } else if (isPost) {
            //printlinef(WHITE, BLACK, "POST request.");
            char *request = con_state->headers + sizeof(HTTP_POST); // + space

            if (strncmp(request, OTA_PATH, sizeof(OTA_PATH) - 1) == 0) {
                // Form submitted
                printlinef(WHITE, BLACK, "Upload POST request.");

                // Parse Content-Length
                int content_len;
                char* find_in = p->payload;
                char* crlfcrlf = strnstr(find_in, CRLF CRLF, p->len);
                if (crlfcrlf != NULL) {
                    char *scontent_len = strnstr(find_in, HTTP_HDR_CONTENT_LEN, crlfcrlf - find_in);
                    if (scontent_len != NULL) {
                        char *scontent_len_end = strnstr(scontent_len + HTTP_HDR_CONTENT_LEN_LEN, CRLF, HTTP_HDR_CONTENT_LEN_DIGIT_MAX_LEN);
                        if (scontent_len_end != NULL) {
                            char *content_len_num = scontent_len + HTTP_HDR_CONTENT_LEN_LEN;
                            content_len = atoi(content_len_num);
                            if (content_len == 0) {
                                /* if atoi returns 0 on error, fix this */
                                if ((content_len_num[0] != '0') || (content_len_num[1] != '\r')) {
                                    content_len = -1;
                                }
                            }
                        }
                    }
                }
                if (content_len <= 0) {
                    printlinef(WHITE, RED, "Could not parse Content-Length header.");
                    con_state->result_len = snprintf(con_state->result, sizeof(con_state->result), HTTP_400);
                    con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_HEADERS, "400 Bad Request", con_state->result_len);
                    send_response(con_state, pcb);
                } else {
                    printlinef(WHITE, BLACK, "Content-Length=%d", content_len);

                    // TODO Body maybe begin right after crlfcrlf in this first tcp pbuf ???

                    // Generate content
                    fw_recv = true;
                    ipaddr_ntoa_r(&pcb->remote_ip, fw_remote_ip, 16);
                    fw_remote_port = pcb->remote_port;
                    fw_total_received = 0;
                    fw_packets_received = 1;
                    fw_flash_ok = true;
                    fw_flash_sector = 0;
                    fw_buffer_filled = 0;
                    fw_total_length = content_len;
                    fw_total_sectors = 1 + ((content_len - 1) / FLASH_SECTOR_SIZE);
                    printlinef(WHITE, BLACK, "fw_total_length=%d fw_total_sectors=%d", fw_total_length, fw_total_sectors);

                    print_progress(false, 0, fw_total_sectors);

                    // No response is sent back, waiting for file upload...
                }

            } else if (strncmp(request, BOOT_PATH, sizeof(BOOT_PATH) - 1) == 0) {
                printlinef(WHITE, GREEN, "Booting stage 2...");
                boot_stage2();
            } else {
                // Not found
                con_state->result_len = snprintf(con_state->result, sizeof(con_state->result), HTTP_404);
                con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_HEADERS, "404 Not Found", con_state->result_len);
                send_response(con_state, pcb);
            }
        }
        
        if (isFirmwareUploadClient) {
            //printlinef(WHITE, RED, "FW CLIENT FOLLOW-UP: len=%d tot_len=%d", p->len, p->tot_len);
            // FIXME HACK Need to parse boundary
            // TODO First body packet --> parse boundary (AND SKIP)
            fw_packets_received++;
            fw_total_received += p->tot_len;
            //printlinef(WHITE, RED, "RECVd: %d / %d", fw_total_received, fw_total_length);
            if (fw_packets_received > 2) {
                // Collect enough packet data to fill 4KB buffer
                uint16_t available = sizeof(fw_buffer) - fw_buffer_filled;
                uint16_t read = p->tot_len > available ? available : p->tot_len;
                uint16_t remaining = p->tot_len - read;
                pbuf_copy_partial(p, fw_buffer + fw_buffer_filled, read, 0);
                fw_buffer_filled += read;
                //printlinef(WHITE, BLACK, "filled=%d", fw_buffer_filled);
                // If buffer is full, erase and program sector
                if (fw_buffer_filled == sizeof(fw_buffer)) {
                    //printlinef(WHITE, GREEN, "PROGRAM SECTOR %d", fw_flash_sector);
                    print_progress(true, fw_flash_sector, fw_total_sectors);
                    fw_flash_ok = fw_flash_ok && (program_flash_sector() == 0);
                    fw_flash_sector++;
                    fw_buffer_filled = 0;
                    print_progress(true, fw_flash_sector, fw_total_sectors);
                }
                // Collect remaining data in packet
                if (remaining > 0) {
                    pbuf_copy_partial(p, fw_buffer + fw_buffer_filled, remaining, read);
                    fw_buffer_filled += remaining;
                    //printlinef(WHITE, BLACK, "filled=%d", fw_buffer_filled);
                }
                
                // FIXME HACK Last packet ends with boundary --> should be ignored. can be flashed anyway since it's after the firmware ¯\(ツ)/¯
                if (fw_total_received >= fw_total_length) {
                    //printlinef(WHITE, RED, "PROGRAM LAST SECTOR %d: len=%d", fw_flash_sector, fw_buffer_filled);
                    print_progress(true, fw_flash_sector, fw_total_sectors);
                    fw_flash_ok = fw_flash_ok && (program_flash_sector() == 0);
                    fw_flash_sector++;
                    fw_buffer_filled = 0;
                    print_progress(false, fw_flash_sector, fw_total_sectors);

                    // Generate result
                    if (fw_flash_ok) {
                        printlinef(WHITE, GREEN, "Upgrade successful!");
                        con_state->result_len = snprintf(con_state->result, sizeof(con_state->result), OTA_BODY_SUCCESS);
                        con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_HEADERS, "200 OK", con_state->result_len);
                        send_response(con_state, pcb);
                    } else {
                        printlinef(WHITE, RED, "Upgrade failed!");
                        con_state->result_len = snprintf(con_state->result, sizeof(con_state->result), OTA_BODY_FAILURE);
                        con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_HEADERS, "200 OK", con_state->result_len);
                        send_response(con_state, pcb);
                    }

                    fw_recv = false;
                }
            }
        }
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_poll(void *arg, struct tcp_pcb *pcb) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    DEBUG_printf("tcp_server_poll_fn\n");
    return tcp_close_client_connection(con_state, pcb, ERR_OK); // Just disconnect clent?
}

static void tcp_server_err(void *arg, err_t err) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    if (err != ERR_ABRT) {
        DEBUG_printf("tcp_client_err_fn %d\n", err);
        tcp_close_client_connection(con_state, con_state->pcb, err);
    }
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    if (err != ERR_OK || client_pcb == NULL) {
        DEBUG_printf("failure in accept\n");
        return ERR_VAL;
    }
    DEBUG_printf("client connected\n");
	//printlinef(WHITE, BLACK, "New TCP client connection.");

    // Create the state for the connection
    TCP_CONNECT_STATE_T *con_state = calloc(1, sizeof(TCP_CONNECT_STATE_T));
    if (!con_state) {
        DEBUG_printf("failed to allocate connect state\n");
        return ERR_MEM;
    }
    con_state->pcb = client_pcb; // for checking
    con_state->gw = &state->gw;

    // setup connection to client
    tcp_arg(client_pcb, con_state);
    tcp_sent(client_pcb, tcp_server_sent);
    tcp_recv(client_pcb, tcp_server_recv);
    tcp_poll(client_pcb, tcp_server_poll, POLL_TIME_S * 2);
    tcp_err(client_pcb, tcp_server_err);

    return ERR_OK;
}

static bool tcp_server_open(void *arg) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    DEBUG_printf("starting server on port %u\n", TCP_PORT);

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        DEBUG_printf("failed to create pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, IP_ANY_TYPE, TCP_PORT);
    if (err) {
        DEBUG_printf("failed to bind to port %d\n");
        return false;
    }

    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!state->server_pcb) {
        DEBUG_printf("failed to listen\n");
        if (pcb) {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_server_accept);

    return true;
}



/*************************************/
/**********   BINARY INFO   **********/
/*************************************/

uint32_t* find_binary_info() {
    for (int i=0x100; i<0x200; i+=4) {
        uint32_t data = *(uint32_t*)(XIP_BASE + FLASH_TARGET_OFFSET + i);
	    //printlinef(WHITE, BLACK, "(1) *(0x%p) = 0x%08x", (uint32_t*)(XIP_BASE + FLASH_TARGET_OFFSET + i), data);
        if (data == BINARY_INFO_MARKER_START) {
            data = *(uint32_t*)(XIP_BASE + FLASH_TARGET_OFFSET + i + 16);
	        //printlinef(WHITE, BLACK, "(2) *(0x%p) = 0x%08x", (uint32_t*)(XIP_BASE + FLASH_TARGET_OFFSET + i + 16), data);
            if (data == BINARY_INFO_MARKER_END) {
                return (uint32_t*)(XIP_BASE + FLASH_TARGET_OFFSET + i);
            }
        }
    }
    return (uint32_t*) 0;
}

void get_firmware_info() {
    uint32_t* binary_info = find_binary_info();
	//printlinef(WHITE, BLACK, "binary_info: %p", binary_info);
    if (binary_info != 0) {
        // Stage 2 firmware exists
        fw_info_slot1.exists = true;
        uint32_t* binary_info_start = (uint32_t*)(binary_info[1]);
        uint32_t* binary_info_end = (uint32_t*)(binary_info[2]);
	    //printlinef(WHITE, BLACK, "binary_info_start: %p", binary_info_start);
	    //printlinef(WHITE, BLACK, "binary_info_end: %p", binary_info_end);
        for (uint32_t* i=binary_info_start; i<binary_info_end; i++) {
	        //printlinef(WHITE, BLACK, "entry @ %p --> 0x%08x", i, *i);
            uint16_t* entry16 = (uint16_t*) *i;
            uint32_t* entry32 = (uint32_t*) *i;
            uint16_t type = entry16[0];
	        //printlinef(WHITE, BLACK, "entry type: 0x%04x", type);
            uint16_t tag = entry16[1];
	        //printlinef(WHITE, BLACK, "entry tag: 0x%04x", tag);
            uint32_t id = entry32[1];
	        //printlinef(WHITE, BLACK, "entry id: 0x%08x", id);
            uint32_t value_addr = entry32[2];
	        //printlinef(WHITE, BLACK, "entry value address: 0x%08x", value_addr);

            switch(id) {
                case BINARY_INFO_ID_RP_PROGRAM_NAME:
                    //printlinef(WHITE, BLACK, "  PROGRAM NAME: %s", (char*)value_addr);
                    fw_info_slot1.prog_name = (char*)value_addr;
                    break;
                case BINARY_INFO_ID_RP_PROGRAM_VERSION_STRING:
                    //printlinef(WHITE, BLACK, "  PROGRAM VERSION: %s", (char*)value_addr);
                    fw_info_slot1.prog_version = (char*)value_addr;
                    break;
                case BINARY_INFO_ID_RP_PROGRAM_BUILD_DATE_STRING:
                    //printlinef(WHITE, BLACK, "  PROGRAM BUILD DATE: %s", (char*)value_addr);
                    fw_info_slot1.prog_date = (char*)value_addr;
                    break;
                case BINARY_INFO_ID_RP_SDK_VERSION:
                    //printlinef(WHITE, BLACK, "  SDK VERSION: %s", (char*)value_addr);
                    fw_info_slot1.sdk_version = (char*)value_addr;
                    break;
                case BINARY_INFO_ID_RP_PICO_BOARD:
                    //printlinef(WHITE, BLACK, "  PICO BOARD: %s", (char*)value_addr);
                    fw_info_slot1.pico_board = (char*)value_addr;
                    break;
            }
        }
    }
}



/*************************************/
/**********    HAND-OVER    **********/
/*************************************/

void disable_interrupts(void)
{
    //printlinef(WHITE, BLACK, "disable_interrupts");
    systick_hw->csr &= ~1;
    nvic_hw->icer = 0xFFFFFFFF;
    nvic_hw->icpr = 0xFFFFFFFF;
}

void reset_peripherals(void)
{
    //printlinef(WHITE, BLACK, "reset_peripherals");
    reset_block(~(
            RESETS_RESET_IO_QSPI_BITS |
            RESETS_RESET_PADS_QSPI_BITS |
            RESETS_RESET_SYSCFG_BITS |
            RESETS_RESET_PLL_SYS_BITS
    ));
}

void jump_to_vtor(uint32_t vtor)
{
    //printlinef(WHITE, BLACK, "jump_to_vtor: 0x%08x", vtor);
	uint32_t reset_vector = *(volatile uint32_t *)(vtor + 0x04);
    //printlinef(WHITE, BLACK, "reset_vector=0x%08x", reset_vector);
    //printlinef(WHITE, BLACK, "msb=0x%08x", (*(volatile uint32_t *)vtor));
    //printlinef(WHITE, BLACK, "bx 0x%08x", reset_vector);
	scb_hw->vtor = (volatile uint32_t)(vtor);
    asm volatile("msr msp, %0"::"g"
            (*(volatile uint32_t *)vtor));
    asm volatile("bx %0"::"r" (reset_vector));
}

void boot_stage2() {
    uint32_t vtor = XIP_BASE + 0x100000 + 0x100;

    // Stop network stuff
    dns_server_deinit(&dns_server);
    dhcp_server_deinit(&dhcp_server);
    tcp_server_close(state);
    cyw43_arch_deinit();

    // Halt core1
    //printlinef(WHITE, BLACK, "multicore_reset_core1");
    multicore_reset_core1();

    // Reset system clock
    //printlinef(WHITE, BLACK, "set_sys_clock_khz: %d KHz", 125000);
    set_sys_clock_khz(125000, true);

    // Reset voltage
    //printlinef(WHITE, BLACK, "vreg_set_voltage(1.1V): %d", VREG_VOLTAGE_DEFAULT);
    vreg_set_voltage(VREG_VOLTAGE_DEFAULT);

    // Disable all interrupts
    disable_interrupts();

    // Reset peripherals
    reset_peripherals();

    // TODO Replace crt0.S
    //  --> copy .data from FLASH to RAM (__data_start__ to __data_end__ ==> __etext)
    //  --> copy .scratch_x from FLASH to RAM (__scratch_x_start__ to __scratch_x_end__ ==> __scratch_x_source__)
    //  --> copy .scratch_y from FLASH to RAM (__scratch_y_start__ to __scratch_y_end__ ==> __scratch_y_source__)
    //  --> clear .bss (__bss_start__ to __bss_end__ = 0)
    // TODO Jump directly to entry_point
    // Point to application vector table, set MSP and jump PC to reset handler
    jump_to_vtor(vtor);
    __builtin_unreachable();
}



int main() {
    // TODO Check for forced upgrade (joybus buttons combination or magic value)
    
    // Check if there is a stage2 firmware. Boot if available
    get_firmware_info();

    /*if (fw_info_slot1.exists) {
        boot_stage2();
    }*/

	vreg_set_voltage(VREG_VSEL);
	sleep_ms(10);
	set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

	setup_default_uart();

	dvi0.timing = &DVI_TIMING;
	dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
	dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

	const int BORDER = 10;
	for (int y = 0; y < FRAME_HEIGHT; ++y)
		for (int x = 0; x < FRAME_WIDTH; ++x)
			putpixel(x, y, (x + y) & 1 ? RED : BLACK);
	for (int y = BORDER; y < FRAME_HEIGHT - BORDER; ++y)
		for (int x = BORDER; x < FRAME_WIDTH - BORDER; ++x)
			putpixel(x, y, WHITE);

	puttext(204, 20, GREEN, BLACK, "Over-the-Air Firmware Upgrade");

	multicore_launch_core1(core1_main);


    if (fw_info_slot1.exists) {
        printlinef(WHITE, BLACK, "Firmware is installed:");
        if (fw_info_slot1.prog_name != 0) {
            printlinef(WHITE, BLACK, "  PROGRAM NAME: %s", fw_info_slot1.prog_name);
        }
        if (fw_info_slot1.prog_version != 0) {
            printlinef(WHITE, BLACK, "  PROGRAM VERSION: %s", fw_info_slot1.prog_version);
        }
        if (fw_info_slot1.prog_date != 0) {
            printlinef(WHITE, BLACK, "  PROGRAM DATE: %s", fw_info_slot1.prog_date);
        }
        if (fw_info_slot1.sdk_version != 0) {
            printlinef(WHITE, BLACK, "  SDK VERSION: %s", fw_info_slot1.sdk_version);
        }
        if (fw_info_slot1.pico_board != 0) {
            printlinef(WHITE, BLACK, "  PICO BOARD: %s", fw_info_slot1.pico_board);
        }
    }

    state = calloc(1, sizeof(TCP_SERVER_T));
    if (!state) {
        DEBUG_printf("failed to allocate state\n");
        return 1;
    }

    if (cyw43_arch_init()) {
        DEBUG_printf("failed to initialise\n");
        return 1;
    }
    const char *ap_name = "ota";
    const char *password = NULL;

	printlinef(WHITE, BLACK, "Creating Wi-FI access point `%s`...", ap_name);
    cyw43_arch_enable_ap_mode(ap_name, password, CYW43_AUTH_WPA2_AES_PSK);
	printlinef(WHITE, GREEN, "Access point `%s` created.", ap_name);

    ip4_addr_t mask;
    IP4_ADDR(ip_2_ip4(&state->gw), 192, 168, 4, 1);
    IP4_ADDR(ip_2_ip4(&mask), 255, 255, 255, 0);

    // Start the dhcp server
	printlinef(WHITE, BLACK, "Starting DHCP server...");
    dhcp_server_init(&dhcp_server, &state->gw, &mask);
	printlinef(WHITE, GREEN, "DHCP server started.");

    // Start the dns server
	printlinef(WHITE, BLACK, "Starting DNS server...");
    dns_server_init(&dns_server, &state->gw);
	printlinef(WHITE, GREEN, "DNS server started.");

	printlinef(WHITE, BLACK, "Starting TCP server...");
    if (!tcp_server_open(state)) {
        DEBUG_printf("failed to open server\n");
        return 1;
    }
	printlinef(WHITE, GREEN, "TCP server started.");
	printlinef(WHITE, BLACK, "Waiting for connection on http://%s%s...", ipaddr_ntoa(&state->gw), OTA_PATH);

	bool blinky = false;

	while (!state->complete) {
		fillrect(1, 1, BORDER / 2, BORDER / 2, (blinky = !blinky) ? GREEN : BLACK);
        // if you are using pico_cyw43_arch_poll, then you must poll periodically from your
        // main loop (not from a timer interrupt) to check for Wi-Fi driver or lwIP work that needs to be done.
        cyw43_arch_poll();
        // you can poll as often as you like, however if you have nothing else to do you can
        // choose to sleep until either a specified time, or cyw43_arch_poll() has work to do:
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(1000));
	}
    dns_server_deinit(&dns_server);
    dhcp_server_deinit(&dhcp_server);
    cyw43_arch_deinit();
	//__builtin_unreachable();

	return 0;

}
