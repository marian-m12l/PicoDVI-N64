#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"

#include <stdio.h>
#include <stdarg.h>

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

void puttextf(uint x0, uint y0, uint bgcol, uint fgcol, const char *fmt, ...) {
	char buf[128];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, 128, fmt, args);
	puttext(x0, y0, bgcol, fgcol, buf);
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

	const uint LABEL_LEFT = 18;

	puttextf(LABEL_LEFT, 40, WHITE, BLACK, "TODO Creating Wi-FI access point...");
	puttextf(LABEL_LEFT, 50, WHITE, BLACK, "TODO Starting DHCP server...");
	puttextf(LABEL_LEFT, 60, WHITE, BLACK, "TODO Starting TCP server...");
	puttextf(LABEL_LEFT, 70, WHITE, BLACK, "TODO Waiting for connection and firmware upload...");
	puttextf(LABEL_LEFT, 80, WHITE, GREEN, "TODO New client connect with IP address 192.168.64.2");
	puttextf(LABEL_LEFT, 90, WHITE, BLACK, "TODO Receiving firmware upload...");
	puttextf(LABEL_LEFT, 100, WHITE, BLACK, "TODO Flashing UF2...");
	puttextf(LABEL_LEFT, 110, WHITE, GREEN, "TODO Succesfully flashed new firmware...");
	puttextf(LABEL_LEFT, 120, WHITE, BLACK, "TODO Booting stage 2 firmware...");

	multicore_launch_core1(core1_main);

	bool blinky = false;

	while (1) {
		__wfi();
	}
	__builtin_unreachable();

}
