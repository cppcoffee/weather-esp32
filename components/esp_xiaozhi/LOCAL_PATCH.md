# Local patch record

This directory is a local override of `espressif/esp_xiaozhi` 0.1.4, based on upstream commit `f9e26eab65969f10e862d19c2a5830527b2136a3`.

- Upstream license: Apache-2.0; the original `license.txt`, SPDX headers and upstream source are retained.
- Modified production files: `include/esp_xiaozhi_info.h`, `src/esp_xiaozhi_info.c` and `Kconfig`.
- Change: add `esp_xiaozhi_chat_get_info_with_url()` and keep `esp_xiaozhi_chat_get_info()` as the compatibility entry point using `CONFIG_XIAOZHI_OTA_URL`. The request, headers, response parsing, activation handling, and transport persistence remain upstream code. `CONFIG_XIAOZHI_OTA_URL` now defaults to an empty string, so the component ships no default server; the panel supplies the URL per device.
- Reason: the weather panel stores the information URL in the Wi-Fi setup portal and must select it at runtime, and the product must not silently talk to a vendor default server.
- Validation: `python3 tools/test_voice_settings.py`, host portal regression, ESP-IDF reconfigure/build, and real-device network/activation/audio behavior still require hardware verification.

The rest of this component remains the single upstream implementation. Do not also enable the registry copy; `main/idf_component.yml` uses `override_path` so Component Manager continues resolving the component's transitive dependencies.
