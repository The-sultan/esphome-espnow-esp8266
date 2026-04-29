# esphome-espnow-esp8266

An ESPHome external component implementing ESP-NOW for ESP8266/ESP8285.

> **Status: early design phase.** No implementation yet.  
> See [docs/design/01-espnow-component.md](docs/design/01-espnow-component.md) for the current design document.

## Why

ESPHome's official ESP-NOW support (introduced in 2025.8) is ESP32-only. This component fills the gap for ESP8266 and ESP8285 devices, using the modern `external_components` mechanism — no `custom_component`, no manual includes, no required lambdas in the high-level API.

## Requirements

- ESPHome >= 2025.2
- ESP8266 or ESP8285, framework: arduino
- Not for ESP32 — use ESPHome's built-in ESP-NOW component instead

## License

MIT
