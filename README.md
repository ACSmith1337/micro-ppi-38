# ESP8266 Micro PPI Radar

An authentic PPI-style radar display for ADS-B data using an ESP8266 and GC9A01 round TFT display. Connects to local readsb/tar1090 or ADSB.lol feeds and displays aircraft with phosphor persistence, beam illumination, and protected HUD overlays.

## Features

- **Authentic PPI radar display** with phosphor persistence and beam sweep
- **Two scan modes:** Angular Sweep and Radial Ping
- **Color-coded aircraft:** green/amber commercial, orange military, red emergency squawk
- **Emergency squawk alerts** for 7500, 7600, 7700, and 1200
- **RSSI-based fade timing** for realistic return persistence
- **Beam illumination** with aircraft revealed by the scan
- **Two phosphor palettes:** green P1 and amber P4
- **Aircraft trail dots** with compressed waypoint history
- **Live configuration reload** without reboot
- **Physical button support** for theme and scan mode changes
- **Dual data sources:** local readsb/tar1090 or ADSB.lol
- **Airport markers** from Overpass API
- **Offline-capable web configuration** with embedded CSS and no CDN dependency
- **SPI-batched rendering** with HUD and alert layers protected from beam overlap

## Hardware Requirements

- ESP8266 NodeMCU
- GC9A01 240x240 round TFT display
- Two optional momentary push buttons
- Micro USB cable for power

## Installation

### Pre-compiled Firmware

1. Download `firmware.bin` from the [releases page](https://github.com/ACSmith1337/micro-ppi-38/releases).
2. Connect the ESP8266 by USB.
3. Flash with esptool:

   ```bash
   esptool.py --port /dev/ttyUSB0 write_flash 0x0 firmware.bin
   ```

4. Power cycle the device.

### Build from Source

Install PlatformIO, then run:

```bash
pio run -e nodemcu
```

The resulting image is:

```text
.pio/build/nodemcu/firmware.bin
```

## Configuration

On first boot, WiFiManager creates the setup access point. Connect to it and complete Wi-Fi configuration. After the device joins the network, open its assigned IP address in a browser.

The local web configuration controls:

- Data source
- Local readsb host, port, and JSON path
- Radar latitude and longitude
- Display range in nautical miles
- Scan mode
- Phosphor palette
- Aircraft information, triangles, trails, and squawk alerts
- Fetch interval

Configuration changes apply live without reboot. The page is embedded in firmware and remains usable without Internet access.

### Diagnostic Endpoints

```text
http://<device-ip>/status   Runtime diagnostics
http://<device-ip>/logs     Device log buffer
http://<device-ip>/sync     Request a data sync
http://<device-ip>/restart  Restart the ESP8266
```

## Physical Buttons

Buttons connect between the GPIO pin and GND. Internal pull-ups are enabled.

| Button | Pin | GPIO | Function |
|---|---|---:|---|
| Theme | D6 | GPIO12 | Green ↔ Amber |
| Scan mode | D4 | GPIO2 | Angular ↔ Radial |

## Wiring

```text
Display Pin | ESP8266 Pin
------------|------------
VCC         | 3.3V
GND         | GND
SCL         | D5 (GPIO14)
SDA         | D7 (GPIO13)
DC          | D2 (GPIO4)
CS          | D8 (GPIO15)
RST         | D3 (GPIO0)
BL          | 3.3V
```

## Scan Modes

### Angular Sweep

A rotating beam sweeps around the display. Aircraft illuminate when the beam crosses their position and then fade according to signal strength.

### Radial Ping

An expanding ring illuminates aircraft as it crosses their position. Cardinal directions, range labels, airport markers, and the squawk indicator are restored above the ping ring.

## Aircraft Fade Behavior

- Weak returns fade faster than strong returns.
- Normal returns use a 64-level brightness scale.
- Emergency squawk returns decay faster.
- Aircraft remain at their last reported API position between data updates.
- Aircraft are drawn below the squawk notification layer.

## Airport Data

Airport requests use an HTTP POST to the Overpass interpreter with this query:

```overpass
[out:json][timeout:20];
nwr["aeroway"="aerodrome"]["iata"](around:RANGE_M,LAT,LON);
out tags center qt;
```

The query searches nodes, ways, and relations. Nodes use direct coordinates. Ways and relations use `center.lat` and `center.lon`.

Requests occur at boot and after latitude, longitude, or range changes. Failed requests wait 60 seconds before retrying.

## Rate Limits

- **ADSB.lol:** one attempt every 60 seconds, including failures
- **Local readsb:** one attempt every 10 seconds
- **Overpass:** one request at boot or after configuration change, with a 60-second retry cooldown

## Aircraft Color Coding

| Type | Squawk range | Color |
|---|---|---|
| Commercial | 0-3999, 5000-6999 | Green or amber |
| Military | 4000-4999, 7000+ | Orange |
| Emergency | 7500, 7600, 7700, 1200 | Red with alert flash |

## Technical Notes

- **Display:** GC9A01 240x240 round TFT
- **Controller:** ESP8266 NodeMCU
- **SPI write speed:** 80 MHz
- **Angular frame interval:** approximately 16 ms
- **Beam rotation:** 10 seconds per sweep
- **Maximum tracked aircraft:** 48
- **Airport coordinates:** cached after fetch
- **Rendering:** one SPI batch per angular frame
- **Static grid redraw:** throttled to reduce ESP8266 workload
- **Firmware artifact:** `bin/firmware.bin`

## Troubleshooting

- Blank display: verify GC9A01 power, ground, SPI, DC, CS, and reset wiring.
- No aircraft: verify the selected data source and radar coordinates.
- No airports: check `/logs`; Overpass requests are rate-limited and may be rejected with HTTP 429.
- Web page unavailable: connect to the device IP or the WiFiManager setup network.
- Display artifacts: check `/logs` and confirm the current firmware is from the latest release.

## Release

Download the current firmware from:

https://github.com/ACSmith1337/micro-ppi-38/releases
