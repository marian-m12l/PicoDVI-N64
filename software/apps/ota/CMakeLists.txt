# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(ota
	main.c
	joybus.c
	dhcpserver/dhcpserver.c
	dnsserver/dnsserver.c
)

target_compile_options(ota PRIVATE -Wall)

target_compile_definitions(ota PRIVATE
	DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
	DVI_VERTICAL_REPEAT=1
	DVI_N_TMDS_BUFFERS=3
	)

target_include_directories(ota PRIVATE
	${CMAKE_CURRENT_LIST_DIR}
	${CMAKE_CURRENT_LIST_DIR}/dhcpserver
	${CMAKE_CURRENT_LIST_DIR}/dnsserver
	${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(ota
	pico_stdlib
	pico_multicore
	libdvi
	pico_cyw43_arch_lwip_poll
	hardware_flash
	hardware_pio
	pico_util
)

# Build pio
pico_generate_pio_header(n64 ${CMAKE_CURRENT_LIST_DIR}/joybus.pio)

# Custom linker script to load critical code to RAM (DVI stuff, ...)
pico_set_binary_type(ota copy_to_ram)	# Needed to set PICO_COPY_TO_RAM and have boot code copy "ram_text" section to RAM
pico_set_linker_script(ota ${CMAKE_CURRENT_SOURCE_DIR}/memmap_dvi_in_ram.ld)

# create map/bin/hex file etc.
pico_add_extra_outputs(ota)
