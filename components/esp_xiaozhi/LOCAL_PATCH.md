# Local patch record

This directory is a local override of `espressif/esp_xiaozhi` 0.1.4, based on upstream commit `f9e26eab65969f10e862d19c2a5830527b2136a3`.

- Upstream license: Apache-2.0; the original `license.txt`, SPDX headers and upstream source are retained.
- Original runtime-URL patch files: `include/esp_xiaozhi_info.h`, `src/esp_xiaozhi_info.c` and `Kconfig`.
- Change: add `esp_xiaozhi_chat_get_info_with_url()` and keep `esp_xiaozhi_chat_get_info()` as the compatibility entry point using `CONFIG_XIAOZHI_OTA_URL`. The request, headers, response parsing, activation handling, and transport persistence remain upstream code. `CONFIG_XIAOZHI_OTA_URL` now defaults to an empty string, so the component ships no default server; the panel supplies the URL per device.
- Reason: the weather panel stores the information URL in the Wi-Fi setup portal and must select it at runtime, and the product must not silently talk to a vendor default server.
- Validation: `python3 tools/test_voice_settings.py`, host portal regression, ESP-IDF reconfigure/build, and real-device network/activation/audio behavior still require hardware verification.

## Bounded WebSocket sends and MCP delivery failures (2026-11-17)

Additional production changes are confined to `src/esp_xiaozhi_websocket.c`,
`src/esp_xiaozhi_mcp.c`, `src/esp_xiaozhi_transport.c`, `src/esp_xiaozhi_chat.c`,
`src/priv_include/esp_xiaozhi_mcp.h` and `include/esp_xiaozhi_chat.h`.

- Reason: a device weather MCP reply is about 3 KB and is sent synchronously from
  the WebSocket receive callback. Upstream used `portMAX_DELAY` for text/binary
  writes and discarded MCP delivery errors, so TCP backpressure could strand the
  receive task and a simultaneous stop/deinit holding the application's network lock.
- Text **and binary** sends now pass finite SDK timeouts and require a complete
  write (`sent == length`); negative, zero-for-nonempty and short writes fail. No
  retry, alternate JSON-RPC implementation or custom WebSocket framing is added.
- Set the SDK RX/TX buffer size to 4096, so the current ~3 KB reply fits one SDK
  fragment. The nominal send wait budget is 5000 ms (below the server's 8000 ms RPC
  deadline), divided by `1 + 3 * ceil(length / 4096)`: one TX-lock wait and, per
  fragment, IDF WS transport poll/header/payload waits. For ~3 KB this is 1250 ms
  per SDK wait, not 5000 ms for each fragment. Larger messages receive smaller
  per-wait timeouts rather than multiplying the budget. Network operation timeout
  is explicitly 5000 ms instead of the SDK default 10000 ms.
- MCP returns response construction/parsing/send failures, always releases the
  manager's response buffer, and preserves successful delivery of JSON-RPC error
  replies as `ESP_OK`. Notifications with no reply and `ESP_OK` remain successful.
  Chat marks `error_occurred` and synchronously reports `CHAT_ERROR` with the error
  code and source `"mcp"`; application code schedules recovery on its own task.
- Connected/disconnected and audio-channel-close event posts are nonblocking.
  A saturated default event loop can drop these notifications instead of blocking
  the receive task or teardown forever. `CHAT_ERROR` uses the direct callback and
  does not depend on that event queue.

### Stop/deinit review and limits

The inspected `esp_websocket_client` SDK implements stop as `run = false`, a
`REQUESTED_STOP_BIT` wakeup and a `STOPPED_BIT` join. The join itself still uses
`portMAX_DELAY`; the public API offers **no timed stop/join**. Stop is rejected on
its own event task. SDK destroy calls that stop helper but ignores its failure.

The adapter now stops/joins before clearing payload state or destroying a client,
tracks successful start/stop for idempotent teardown, and checks destroy results.
Transport and MCP stop propagate errors; chat does not call the manager's deinit
(which ignores transport teardown errors), unregister or free itself after a
failed transport stop. The retained handle can be retried from a non-callback task.
No forced task deletion, detached cleanup task, timeout-and-free, or downloaded
SDK modification is used: callback-owned resources must not become dangling.

This removes this component's infinite send wait and event-queue wait on the
reviewed path, **not a hard wall-clock guarantee of a 5 s send or bounded total
deinit**. SDK/TLS internal retries or partial progress, scheduler delays, other
in-flight SDK network operations, DNS and application/tool/event callbacks can add
time. In particular application callbacks must return promptly and must not take
an application lock held by teardown. The nominal wait accounting assumes the
inspected SDK/IDF WS implementation and must be rechecked on dependency upgrades.
A strict end-to-end <8 s RPC or timed safe join would require upstream SDK support;
it cannot safely be promised by changing the send timeout alone.

Resource/behavior tradeoffs: 4096-byte RX and TX buffers use about 6 KB more total
buffer memory than the SDK's 1024-byte default (subject to SDK dynamic-buffer
configuration). Congested audio and large messages fail earlier rather than
waiting indefinitely. The server may still time out a failed RPC; no automatic
MCP response replay is attempted.

### Regression coverage and validation

- Added `test_apps/main/test_send_backpressure.c` using Unity and test-only linker
  wrappers of the WebSocket SDK and MCP manager response APIs. Outside these cases
  wrappers forward to the real APIs. Real chat/MCP envelope/transport/adapter code
  is exercised; no real server or Wi-Fi is required.
- Covers ~3 KB full delivery, negative/zero/short sends, adapter return propagation,
  response release, valid JSON-RPC error-reply delivery, no-response notifications
  and processing failures, exactly one MCP chat-error callback, callback-context
  stop rejection, retained chat for safe retry, finite binary writes and idempotent
  stop-before-destroy ordering. The stop stub models join success/failure; it does
  not measure real SDK shutdown latency or TCP backpressure.
- `test_apps/main/CMakeLists.txt` adds private headers/direct test dependencies and
  test-only linker wrap flags. `test_apps/CMakeLists.txt` no longer requires an
  absent sibling `components/mcp-c-sdk`; Component Manager resolves that dependency
  from this local component's manifest.
- Validation performed: ESP32-S3 GCC `-fsyntax-only` using the existing ESP-IDF
  v6.0.2 compilation database passed for all four changed production C files and
  the new Unity test translation unit. `git diff --check` passed. These are compile
  checks, **not executed Unity test results**.
- Host execution is not supported by this SDK test application (FreeRTOS/ESP-IDF
  runtime and upstream 32-bit pointer handles). The independent test app now builds
  successfully (see build-only validation below); on-device/QEMU execution of
  `[backpressure]` remains pending. The containing
  firmware's full build is left to the integrator; it does not compile `test_apps`.
  Real WSS/TLS TCP backpressure, concurrent stop while a response is pending, audio
  recovery and application network-lock/weather latency require hardware testing.

### Follow-up: explicit WS two-phase teardown contract and SDK 2.0.2

Only public-header documentation and test files/configuration were changed in this
follow-up; production C behavior is unchanged.

- Documented the application pattern: serialize all chat API use on the owning
  application task; call `chat_stop` first; on failure keep chat, caller-owned MCP
  engine and callback/audio resources alive, defer retry, release application locks
  between attempts and do not start/send a new session. Callbacks may still run
  while cleanup is pending. Never hold a lock needed by a callback across stop.
- After WS `chat_stop` returns `ESP_OK`, with no restart/concurrent API calls,
  `chat_deinit` consumes the chat handle even if it reports a cleanup error. Clear
  the caller's handle after this call and do not retry it. Neither function destroys
  a caller-owned MCP engine (`owns_mcp_engine=false`). Application-owned audio
  tasks/queued consumers need their own quiescence before releasing Opus/context;
  a WebSocket join does not join those tasks or drain the app's event queue.
- A deinit error without that preceding successful stop is ambiguous: stop failure
  retains the chat, but owned-engine cleanup status is returned after freeing chat.
  This is why the checked-stop-first pattern is recommended, rather than deciding
  handle ownership from a deinit error alone. This statement is WS-specific, not a
  guarantee about the separate MQTT/UDP cleanup paths.
- Checked registry-original `mcp-c-sdk 2.0.2`, commit
  `f9e26eab65969f10e862d19c2a5830527b2136a3`: manager stop propagates the transport
  result; manager deinit ignores transport teardown errors and frees the manager;
  engine destroy returns `ESP_OK` for a valid engine after teardown. In the normal
  serialized WS/caller-owned-engine case, deinit after successful stop is therefore
  expected to return `ESP_OK`. The owned-cleanup-error regression injects a status
  after real engine destruction to exercise chat's existing consume-on-error path;
  it does not claim the original 2.0.2 engine normally returns that status.
- Added tests for WS stop failure/retry with a retained, usable external MCP engine,
  idempotent successful stop, no repeated SDK stop during later deinit, exactly-once
  caller-owned engine destruction, and an owned-engine cleanup error after stop
  that still consumes/unregisters chat. Test wrappers now also wrap `esp_mcp_destroy`.
- Added test-only `test_apps/main/idf_component.yml` pinning MCP SDK to `==2.0.2`;
  the test app must not silently validate against 2.1.0 via the component's `^2.0.2`
  range. The containing application's dependency pin/lock remains integrator-owned.
- Validation: downloaded the official registry 2.0.2 archive into a temporary test
  directory (archive SHA256
  `7906a50c3ab5897ea98b0fbaa2b86f8bed89fe80024ee955f822a2ac3a82c716`), inspected
  teardown code, and substituted its include/private paths in ESP32-S3 GCC syntax
  checks. All four patched production C files, both Unity translation units and
  original 2.0.2 manager/engine source passed. The temporary SDK copy was removed;
  `managed_components/` was not modified. Those were syntax-only checks; the full
  test-app build is recorded below. Unity execution remains pending.

### Independent test-app build validation (ESP-IDF 6.0.2, ESP32-S3)

- Completed a full `idf.py build` of `test_apps`, using the IDF 6.0.2 Python venv
  and the installed Xtensa toolchain prepended to PATH. An initial invocation
  failed because the compiler was absent from PATH; adding the existing toolchain
  directory resolved it without any test-config or source changes.
- Build output is in root `.cache/esp-xiaozhi-tests/build`; sdkconfig is
  `.cache/esp-xiaozhi-tests/sdkconfig`. Component Manager resolved registry MCP SDK
  **2.0.2**. Its generated test-app lockfile was archived afterward at
  `.cache/esp-xiaozhi-tests/dependencies.lock` to avoid adding a generated lockfile
  under the test project. Downloads remain in the ignored test-app
  `managed_components/`; no downloaded source was patched.
- `esp_xiaozhi_test.elf` linked successfully. Inspected its symbols/test strings to
  confirm the new backpressure/two-phase-teardown cases and SDK/MCP wrappers are
  included. Generated `esp_xiaozhi_test.bin` is `0xac270` bytes and fits the 1 MiB
  app partition with 33% free space. No `test_utils` or linker-flag fix was needed.
- Build emitted a nonfatal warning about unset `ESP_ROM_ELF_DIR` for GDB ROM symbols;
  binary generation and partition size checks succeeded. No flash, monitor, QEMU
  or hardware execution was performed: this confirms compilation/linking, not
  passing Unity assertions or real TCP/TLS shutdown behavior.

Everything else remains the single upstream implementation. Do not also enable
the registry copy; `main/idf_component.yml` uses `override_path` so Component Manager
continues resolving the component's transitive dependencies. No `main/`, `tools/`,
application lockfile, production manifest or `managed_components/` changes are part
of this patch (the independent test app has its own exact-version manifest).
