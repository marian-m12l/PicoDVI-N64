N64 digital video out using PicoDVI
===========================================

# Hardware

You will need a Raspberry Pi Pico with a DVISock.

The digital video signals need to be wired to the Pico per the wiring diagram below.

![](img/n64-kicad-NUS-CPU-P-01-PicoDVI.png)
![](img/wiring.jpg)

# Building

To build the software, first ensure you have a working [Pico SDK](https://github.com/raspberrypi/pico-sdk) setup.

```
cd software
mkdir build
cd build
cmake ..
make n64
```

You should now have a file `apps/n64/n64.uf2` which you can flash the Pico with.

# Using the Over-the-Air updater/bootloader

Requires a Pico W.

You can install a bootloader as the primary firmware. PicoDVI-N64 firmware must be built as a "stage 2" payload.

The bootloader will hand over to the installed firmware if available. Otherwie (or when triggered at startup by holding `Z + START` on the N64 controller) the bootloader will start a Wi-Fi access point. Connect to the `ota` SSID and open http://192.168.4.1/ota (if not automatically redirected) to upload a freshly built n64 firmware. **The bootloader expects the _bin_ file: `apps/n64/n64.bin`.**

``` 
cd software
mkdir build
cd build
cmake --fresh -DWITH_OTA_BOOTLOADER=1 -DPICO_BOARD=pico_w ..
make n64
make ota
```

You should now have 2 files `apps/ota/ota.uf2` and `apps/n64/n64.uf2` which you can flash the Pico with.

# Pre-built binaries

This repo is using GitHub Actions to build automatically. You can find the binary in the Actions tab. (Actions -> Select the latest build -> Scroll down and download the artifact "PicoDVI-N64" which is a zip file containing the .uf2)