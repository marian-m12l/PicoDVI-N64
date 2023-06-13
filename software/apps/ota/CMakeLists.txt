# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(ota
	main.c
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
)

target_link_libraries(ota
	pico_stdlib
	pico_multicore
	libdvi
	pico_cyw43_arch_lwip_poll
)

# create map/bin/hex file etc.
pico_add_extra_outputs(ota)
