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

#include "font_8x8.h"
#define FONT_CHAR_WIDTH 8
#define FONT_CHAR_HEIGHT 8
#define FONT_N_CHARS 95
#define FONT_FIRST_ASCII 32

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
	char buf[128];
	vsnprintf(buf, 128, fmt, args);
	puttext(x0, y0, bgcol, fgcol, buf);
}

void puttextf(uint x0, uint y0, uint bgcol, uint fgcol, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	puttextf_valist(x0, y0, bgcol, fgcol, fmt, args);
	va_end(args);
}


const uint LABEL_LEFT = 18;
uint16_t line = 40;

void printlinef(uint bgcol, uint fgcol, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	puttextf_valist(LABEL_LEFT, line, bgcol, fgcol, fmt, args);
	line += 10;
	va_end(args);
}


struct dvi_inst dvi0;

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


// Firmware is flashed at a 1MB offset (0x100000)
#define FLASH_TARGET_OFFSET (1024 * 1024)
const uint8_t *flash_target_contents = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET);

void print_buf(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        printf("%02x", buf[i]);
        if (i % 16 == 15)
            printf("\n");
        else
            printf(" ");
    }
}

bool program_flash() {
	// FIXME get data as param
    uint8_t data[FLASH_PAGE_SIZE];
    for (int i = 0; i < FLASH_PAGE_SIZE; ++i)
        data[i] = i % 128;

    printf("Generated data:\n");
    print_buf(data, FLASH_PAGE_SIZE);

    // Note that a whole number of sectors must be erased at a time.
    printf("\nErasing target region...\n");
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    printf("Done. Read back target region:\n");
    print_buf(flash_target_contents, FLASH_PAGE_SIZE);

    printf("\nProgramming target region...\n");
    flash_range_program(FLASH_TARGET_OFFSET, data, FLASH_PAGE_SIZE);
    printf("Done. Read back target region:\n");
    print_buf(flash_target_contents, FLASH_PAGE_SIZE);

    bool mismatch = false;
    for (int i = 0; i < FLASH_PAGE_SIZE; ++i) {
        if (data[i] != flash_target_contents[i])
            mismatch = true;
    }
    if (mismatch)
        printf("Programming failed!\n");
    else
        printf("Programming successful!\n");
	
	return mismatch;
}


#define TCP_PORT 80
#define DEBUG_printf printf
#define POLL_TIME_S 5
#define HTTP_GET "GET"
#define HTTP_POST "POST"
#define HTTP_RESPONSE_HEADERS "HTTP/1.1 %d OK\nContent-Length: %d\nContent-Type: text/html; charset=utf-8\nConnection: close\n\n"
#define OTA_BODY "<html><body><h1>Over-the-Air Firmware Upgrade.</h1><form method=\"POST\" action=\"%s\"><input type=\"file\" id=\"firmware\" name=\"firmware\" accept=\"uf2\" /><input type=\"submit\" value=\"Upgrade\" /></form></body></html>"
#define OTA_BODY_SUCCESS "<html><body><h1>Over-the-Air Firmware Upgrade.</h1><h2>Upgrade successful!</h2></body></html>"
#define OTA_BODY_FAILURE "<html><body><h1>Over-the-Air Firmware Upgrade.</h1><h2>Upgrade Failed!</h2></body></html>"
#define FIRMWARE_PARAM "firmware=%d"
#define OTA_PATH "/ota"
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
    char result[256];
    int header_len;
    int result_len;
    ip_addr_t *gw;
} TCP_CONNECT_STATE_T;

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

static int test_server_content(const char *request, const char *params, char *result, size_t max_result_len) {
    int len = 0;
    if (strncmp(request, OTA_PATH, sizeof(OTA_PATH) - 1) == 0) {
        // Generate result
        len = snprintf(result, max_result_len, OTA_BODY, OTA_PATH);
    }
    return len;
}

static int test_server_content_upload(const char *request, const char *params, char *result, size_t max_result_len) {
    int len = 0;
    if (strncmp(request, OTA_PATH, sizeof(OTA_PATH) - 1) == 0) {
		printlinef(WHITE, BLACK, "Upload POST request.");
        // See if the user uploaded a firmware
		// TODO Handle file upload
		// TODO program flash
		// TODO read from programmed flash ??
		bool isUpgradeOk = program_flash() == 0;

		printlinef(WHITE, BLACK, "Programmed flash with value: 0x%08x", *((uint32_t*)flash_target_contents));

        // Generate result
        if (isUpgradeOk) {
			printlinef(WHITE, GREEN, "Upgrade successful!");
            len = snprintf(result, max_result_len, OTA_BODY_SUCCESS);
        } else {
			printlinef(WHITE, RED, "Upgrade failed!");
            len = snprintf(result, max_result_len, OTA_BODY_FAILURE);
        }
    }
    return len;
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
#if 0
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            DEBUG_printf("in: %.*s\n", q->len, q->payload);
        }
#endif
        // Copy the request into the buffer
        pbuf_copy_partial(p, con_state->headers, p->tot_len > sizeof(con_state->headers) - 1 ? sizeof(con_state->headers) - 1 : p->tot_len, 0);

        // Handle GET and POST requests
		bool isGet = strncmp(HTTP_GET, con_state->headers, sizeof(HTTP_GET) - 1) == 0;
		bool isPost = strncmp(HTTP_POST, con_state->headers, sizeof(HTTP_POST) - 1) == 0;
        if (isGet || isPost) {
        	if (strncmp(HTTP_GET, con_state->headers, sizeof(HTTP_GET) - 1) == 0) {
				//printlinef(WHITE, BLACK, "GET request.");
				char *request = con_state->headers + sizeof(HTTP_GET); // + space
				char *params = strchr(request, '?');
				if (params) {
					if (*params) {
						char *space = strchr(request, ' ');
						*params++ = 0;
						if (space) {
							*space = 0;
						}
					} else {
						params = NULL;
					}
				}

				// Generate content
				con_state->result_len = test_server_content(request, params, con_state->result, sizeof(con_state->result));
				DEBUG_printf("Request: %s?%s\n", request, params);
				//printlinef(WHITE, BLACK, "Request: %s?%s", request, params);
				DEBUG_printf("Result: %d\n", con_state->result_len);
				//printlinef(WHITE, BLACK, "Response: %d", con_state->result_len);
			} else { // isPost
				//printlinef(WHITE, BLACK, "POST request.");
				char *request = con_state->headers + sizeof(HTTP_POST); // + space
				char *params = strchr(request, '?');	// TODO Get uploaded file in body!
				if (params) {
					if (*params) {
						char *space = strchr(request, ' ');
						*params++ = 0;
						if (space) {
							*space = 0;
						}
					} else {
						params = NULL;
					}
				}

				// TODO pbuf* p contains body ??? Need to handle multiple/follow-up TCP packets?

				// Generate content
				con_state->result_len = test_server_content_upload(request, params, con_state->result, sizeof(con_state->result));
				DEBUG_printf("Request: %s?%s\n", request, params);
				//printlinef(WHITE, BLACK, "Request: %s?%s", request, params);
				DEBUG_printf("Result: %d\n", con_state->result_len);
				//printlinef(WHITE, BLACK, "Response: %d", con_state->result_len);
			}

			// Check we had enough buffer space
			if (con_state->result_len > sizeof(con_state->result) - 1) {
				DEBUG_printf("Too much result data %d\n", con_state->result_len);
				return tcp_close_client_connection(con_state, pcb, ERR_CLSD);
			}

			// Generate web page
			if (con_state->result_len > 0) {
				con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_HEADERS,
					200, con_state->result_len);
				if (con_state->header_len > sizeof(con_state->headers) - 1) {
					DEBUG_printf("Too much header data %d\n", con_state->header_len);
					return tcp_close_client_connection(con_state, pcb, ERR_CLSD);
				}
			} else {
				// Send redirect
				con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_REDIRECT,
					ipaddr_ntoa(con_state->gw));
				DEBUG_printf("Sending redirect %s", con_state->headers);
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


int main() {
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

	//printlinef(LABEL_LEFT, 90, WHITE, BLACK, "TODO Receiving firmware upload...");
	//puttextf(LABEL_LEFT, 100, WHITE, BLACK, "TODO Flashing UF2...");
	//puttextf(LABEL_LEFT, 110, WHITE, GREEN, "TODO Succesfully flashed new firmware...");
	//puttextf(LABEL_LEFT, 120, WHITE, BLACK, "TODO Booting stage 2 firmware...");

	multicore_launch_core1(core1_main);



    TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
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
    dhcp_server_t dhcp_server;
	printlinef(WHITE, BLACK, "Starting DHCP server...");
    dhcp_server_init(&dhcp_server, &state->gw, &mask);
	printlinef(WHITE, GREEN, "DHCP server started.");

    // Start the dns server
    dns_server_t dns_server;
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
