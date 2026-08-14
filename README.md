# Micro PPI 38

ESP8266 aircraft PPI radar firmware for a GC9A01 240x240 round display.

This is a clean current-code snapshot. It is not based on OpenSky and contains no legacy enclosure, drawing, test, or historical project material.

## Hardware

- ESP8266 NodeMCU
- GC9A01 240x240 round display
- LovyanGFX

## Build

```bash
pio run -e nodemcu
```

Firmware output:

```text
.pio/build/nodemcu/firmware.bin
```

A matching firmware image is included at `bin/firmware.bin`.

## Features

- Angular sweep and radial ping modes
- Green and amber phosphor themes
- Aircraft persistence and RSSI-based fading
- Squawk and military aircraft indicators
- Local readsb/tar1090 data
- ADSB.lol data with a 60-second attempt limit
- Overpass airport markers with a 60-second request limit
- Offline-capable embedded configuration page
- Batched SPI rendering with protected HUD and alert layers

## Overpass query

The ESP8266 sends an HTTP POST with this URL-encoded `data=` query:

```overpass
[out:json][timeout:20];
nwr["aeroway"="aerodrome"]["iata"](around:RANGE_M,LAT,LON);
out tags center qt;
```

Airport nodes use direct `lat` and `lon`. Airport ways and relations use `center.lat` and `center.lon`.

## ESP8266 display wiring

| GC9A01 | NodeMCU |
|---|---|
| SCL/SCK | D5 / GPIO14 |
| SDA/MOSI | D7 / GPIO13 |
| DC | D2 / GPIO4 |
| CS | D8 / GPIO15 |
| RST | D3 / GPIO0 |
| MISO | D6 / GPIO12, unused by the display |
| VCC | 3.3V |
| GND | GND |

The display driver uses 80 MHz SPI. The round-panel backlight is tied to VCC.

## First boot

If saved Wi-Fi credentials are unavailable, WiFiManager starts a setup access point. Complete Wi-Fi setup, then open the device IP address in a browser. mDNS is also enabled at `http://microradar.local/` when the network resolves it.

## Web endpoints

- `/` configuration page
- `/api/config` persisted configuration JSON
- `/save` apply configuration changes
- `/sync` request an aircraft sync
- `/logs` device logs
- `/status` heap, uptime, Wi-Fi, and firmware status
- `/restart` restart the device

## Runtime constraints

- Overpass airport requests occur at boot and after latitude, longitude, or range changes. Failed requests wait 60 seconds before retrying.
- ADSB.lol attempts are limited to one every 60 seconds, including failures.
- Local readsb attempts are limited to one every 10 seconds.
- The display uses one SPI batch per angular frame. HUD and alert layers are restored above beam, trail, aircraft, and ping rendering.
- `.pio/` is intentionally ignored. Commit only source, configuration, documentation, license, and the release firmware artifact.

## Flashing

Build locally with PlatformIO and flash the resulting image with an ESP8266-compatible esptool command. The release `bin/firmware.bin` is already built for the NodeMCU target.

## Release

Download firmware from:

https://github.com/ACSmith1337/micro-ppi-38/releases
