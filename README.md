# Firmware Update Manager

A C firmware update simulator with three FreeRTOS tasks, a W25Q64JV SPI NOR driver, and persistent image slots. A Python sender packages an image; the receiver validates it before allowing a trial boot. An unconfirmed trial rolls back on the next boot.

Runs on Linux or WSL without a board. `boot` selects an image and updates metadata; it does not execute the image. The example files are small test payloads.

## Build and test

Requires GCC, Make, Git, CMake 3.24+, and Python 3.10+. CMake downloads pinned versions of FreeRTOS V11.2.0 and Unity v2.6.1.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Try an update

```sh
demo_dir=$(mktemp -d)
build/fw_update init "$demo_dir/device.flash"
python3 tools/sender.py examples/v1.txt --version 1 --output "$demo_dir/v1.packet"
build/fw_update upload "$demo_dir/device.flash" "$demo_dir/v1.packet"
build/fw_update boot "$demo_dir/device.flash"
build/fw_update confirm "$demo_dir/device.flash" 1

python3 tools/sender.py examples/v2.txt --version 2 --output "$demo_dir/v2.packet"
build/fw_update upload "$demo_dir/device.flash" "$demo_dir/v2.packet"
build/fw_update boot "$demo_dir/device.flash"
build/fw_update boot "$demo_dir/device.flash"
build/fw_update status "$demo_dir/device.flash"
```

The first version is confirmed. Version 2 gets one trial boot, then the next `boot` selects version 1 because version 2 was never confirmed. Each command reports JSON. Images may be 1 byte to 256 KiB; versions must increase from the confirmed version.

## How it works

| Task | Priority | Responsibility |
| --- | --- | --- |
| Receiver | 1 | Parse framed input and block when the four-packet queue is full |
| Controller | 2 | Check sequence numbers, write the inactive slot, verify CRC, commit metadata |
| Storage | 3 | Own the SPI flash model and persist each erase or page program |

The upload runs on the real FreeRTOS POSIX port with queues, event groups, and delayed busy polling. Two metadata sectors preserve the previous committed record during an update. See [design and test notes](docs/design.md) for the protocol, flash layout, and recovery rules.

## Failure testing

Add `--cut-after N --tear-bytes M` to `upload`, `boot`, or `confirm` to interrupt the Nth erase/program operation after applying its first M bytes. `--stuck-busy` exercises polling timeouts. Open the same flash file in another command to inspect recovery.

The suite contains 43 C tests, 22 Python tests, and one runtime smoke test, including parameterized interruption cases, malformed packets, image boundaries, rollback, file locking, and process restarts. CI also runs the C suites with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DBUILD_RUNTIME=OFF -DENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

CRC detects accidental corruption; updates are not authenticated. Recovery tests use deterministic simulated flash failures, not physical power cuts. A confirmed image provides the rollback target; interrupted first-time provisioning can fail closed with no valid metadata. STM32 startup, UART transport, and hardware timing are outside this simulator.
