# ESP Xiaozhi Component Test Application

This directory contains test applications for the ESP Xiaozhi component.

## Test Structure

- `main/test_esp_xiaozhi.c` - Main test file containing unit tests for ESP Xiaozhi functionality
- `main/test_send_backpressure.c` - `[backpressure]` tests using test-only SDK/linker wrappers; real chat, MCP response wrapping and transport code
- `sdkconfig.defaults` - Default configuration for the test application

## Test Cases

The test application includes the following test cases:

1. **esp_xiaozhi_init_deinit** - Tests basic component initialization and deinitialization
2. **esp_xiaozhi_init_null_config** - Tests error handling with NULL config
3. **esp_xiaozhi_init_null_handle** - Tests error handling with NULL handle
4. **esp_xiaozhi_deinit_null_handle** - Tests error handling with NULL handle in deinit

## Building and Running

To build the test application:

```bash
cd components/esp_xiaozhi/test_apps
idf.py build
```

To run the tests:

```bash
idf.py -p <PORT> flash monitor
```

Then press **ENTER** to open the test menu, and type a number or `*` to run tests.
Use `[backpressure]` to select the new fault-injection group.

That group checks ~3 KB MCP delivery with a nominal 5 s wait budget, negative/zero/
short writes, failure return propagation and `CHAT_ERROR`, response ownership,
JSON-RPC error replies and notifications, binary-send timeouts, stop failure and
safe teardown retry. Additional cases check the application two-phase WS pattern:
failed stop retains the caller-owned MCP engine; successful stop followed by deinit
consumes chat without another SDK stop; an injected owned-engine cleanup error
still consumes chat. The engine error wrapper calls real destroy before injecting
the return status, so this is not a fake-engine lifetime test.

SDK and MCP response/engine wrappers are enabled only within those
cases and forward to the real APIs otherwise. No Wi-Fi or server is needed, but
these are ESP-IDF/Unity tests, **not host-executable tests**. They write a dummy
WebSocket URL to test NVS; use a dedicated test device/image.

The SDK stop stub models join completion/rejection; it cannot prove real TCP/TLS
latency or safe shutdown timing. Test real WSS backpressure and concurrent teardown
on hardware as well. The SDK stop API has no timed join; retained resources must
not be forcibly freed while its callback/task may still be using them. See
`../LOCAL_PATCH.md` for implementation scope and remaining timing limits.

Building the containing weather firmware does not compile this test application;
build it separately with the commands above. The local component manifest resolves
`mcp-c-sdk`/WebSocket SDK through Component Manager; no sibling source copy is needed.
`main/idf_component.yml` pins `espressif/mcp-c-sdk` to **exactly 2.0.2**, independently
of the containing firmware's lockfile. Verify that version in this test app's
resolved lockfile; do not substitute 2.1.0 or patch `managed_components/`.

After successful WS stop, deinit consumes chat even if reporting a cleanup error;
do not retry that old handle. A failed stop leaves resources alive for a later
retry and does not guarantee callbacks have ceased. Application audio workers and
queued events must be quiesced separately before their contexts are freed. See
`esp_xiaozhi_chat.h` for the full ownership and serialization contract.

If you see **"Writing to serial is timing out"**: ensure the monitor is connected to the same port used as the application console. For ESP32-S2/S3/C2/C6 boards with a single USB cable, `sdkconfig.defaults` sets the console to **USB Serial/JTAG** so the same port works for both flashing and the interactive menu. After changing `sdkconfig.defaults`, run `idf.py fullclean && idf.py build` so the new console setting takes effect.

## CI Integration

The test application can be integrated into the CI pipeline and will run automatically when:
- ESP Xiaozhi component code is modified
- The test application itself is modified
- Manual CI trigger

## Notes

- All tests use Unity test framework
- Tests are designed to be run on actual hardware or QEMU
- Memory leak detection is enabled for all tests
- Tests verify proper resource cleanup
