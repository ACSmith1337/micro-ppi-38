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

## Configuration

The device hosts its own configuration page. CSS and behavior are embedded in firmware, so the page works without an Internet route.
