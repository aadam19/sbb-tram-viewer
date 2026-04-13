# SBB Transport Display (ESP32 + LVGL)

Touch UI for an ESP32 display that shows:
- an analog clock screen,
- station search (Zurich dataset),
- live departures from Swiss OJP API,
- Wi-Fi setup and saved Wi-Fi reconnect,
- persistent station favorites,
- startup splash logo.

## Features

- Clock screen with SBB-style dial and second marker.
- Station search with top results (max 10), favorites first.
- Favorites persisted in NVS (`Preferences`) across reboot/power loss.
- Live departures fetched from OJP (OpenTransportData Swiss).
- Automatic refresh of selected station departures every 30 seconds.
- Startup splash logo from `sbb.png` converted to `logo.c`.

## Requirements

- Arduino IDE 2.x
- ESP32 Arduino core (2.x line; tested with ESP32-S3 toolchain from Arduino package manager)
- LVGL `8.4.0` (project is written for LVGL v8 APIs)
- Board support/libraries from this project tree:
  - `ESP32_Display_Panel` stack used by `display.*`, `esp_bsp.*`, `lv_port.*`
- Python 3 (optional, only needed when regenerating assets like `logo.c` or station header files)

Recommended:
- Use the same LVGL major/minor (`8.4.x`) to avoid API/style behavior differences.

## Project Structure

- `DEMO_LVGL.ino`: app startup, Wi-Fi auto-connect, NTP sync, splash screen.
- `ui.cpp`: UI logic, station search, favorites, OJP request/parse, departures list.
- `logo.c`: LVGL image asset generated from `sbb.png`.
- `stations_data.h`: generated station dataset used by search.
- `lv_port.c/.h`, `display.*`, `esp_bsp.*`: board/display/LVGL plumbing.

## API Used

### OJP Stop Event API

- Endpoint: `https://api.opentransportdata.swiss/ojp20`
- Method: `POST`
- Content-Type: `application/xml; charset=utf-8`
- Accept: `application/xml`
- Auth header: `Authorization: Bearer <token>`

Token is configured in `ui.cpp`:
- macro `OJP_API_TOKEN`

Request type sent:
- `OJPStopEventRequest`
- Includes:
  - `StopPointRef` (station number from dataset),
  - `Name`,
  - `NumberOfResults` (currently 8),
  - `StopEventType` (`departure`).

Parsed response fields:
- line: `PublishedServiceName/Text` (fallback to `PublishedLineName` / `PublicCode`)
- destination: `DestinationText/Text` (fallbacks included)
- time: `EstimatedTime` (fallback `TimetabledTime`)

## Station Dataset

`stations_data.h` is generated from `stop-points-today.json` and currently filtered to canton `ZH`.

Each entry contains:
- `number` (station id),
- `designationofficial`,
- `means_of_transport`,
- `localityname`,
- `municipalityname`.

Search UI displays:
- left: `designationofficial`
- right: `means_of_transport`

## Build / Flash

1. Open folder in Arduino IDE.
2. Select your ESP32 board and correct partition/flash settings.
3. Ensure LVGL dependency is installed (v8).
4. Verify and upload.

Serial monitor shows debug logs for:
- Wi-Fi,
- OJP request/response (if enabled in code),
- app startup milestones.

## Configuration Notes

- Time sync: `configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", ...)`
- Wi-Fi reconnect at boot: enabled in `DEMO_LVGL.ino`.
- Favorites persistence namespace/key:
  - namespace: `ui_state`
  - key: `fav_bits`

## Regenerating Logo Asset

Current workflow:
- place/update `sbb.png` in project root,
- convert to `logo.c` (LVGL `lv_img_dsc_t` symbol `logo_img`).

## Regenerating Station Data

Regenerate `stations_data.h` from source JSON when dataset changes:
- filter canton as needed,
- keep fields used by `station_t` in `ui.cpp`.
