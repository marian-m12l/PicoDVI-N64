# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(n64
	main.c
)

target_compile_definitions(n64 PRIVATE
	DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
	)

target_link_libraries(n64
	pico_stdlib
	pico_multicore
	pico_util
	libdvi
	libsprite
	hardware_pio
)

# Build pio
pico_generate_pio_header(n64 ${CMAKE_CURRENT_LIST_DIR}/n64.pio)
  
# If the firmware is run from the OTA bootloader, use custom linker script to move stage 2 firmware to 1MB offset in flash
if (DEFINED CACHE{WITH_OTA_BOOTLOADER})
	pico_set_linker_script(n64 ${CMAKE_CURRENT_SOURCE_DIR}/memmap_offset.ld)
endif()

# create map/bin/hex file etc.
pico_add_extra_outputs(n64)
