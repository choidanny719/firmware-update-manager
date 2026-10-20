# Design notes

## Scope and implementation

This project exercises an embedded update workflow without physical hardware. The update logic and SPI driver are C11 modules. A file-backed NOR model supplies the SPI transport, and the FreeRTOS POSIX port runs the upload pipeline on Linux. The CLI loads a packet file before starting the scheduler, so host file reads do not stand in for an interrupt-driven UART.

Implementation was split into SPI storage, recoverable updates, the RTOS pipeline, integration tests, and documentation/CI. Each stage has its own commit.

| Directory | Contents |
| --- | --- |
| `include`, `src` | SPI driver, CRC, packet parser, metadata journal, update state machine |
| `sim` | NOR command model and persistent flash file |
| `app` | CLI, FreeRTOS configuration, three application tasks |
| `tools` | Python image packager |
| `tests` | Unity unit tests and Python process-level tests |

## SPI device

The driver follows the [Winbond W25Q64JV datasheet, revision C](https://media.digikey.com/pdf/Data%20Sheets/Winbond%20PDFs/W25Q64JV_Rev_C.pdf): 8 MiB capacity, 256-byte pages, and 4 KiB erase sectors. The expected JEDEC ID is `EF 40 17`.

| Command | Opcode | Use |
| --- | --- | --- |
| Read JEDEC ID | `9F` | Reject unsupported devices |
| Read status register 1 | `05` | Poll BUSY and inspect WEL |
| Write enable | `06` | Enable each program or erase |
| Read data | `03` | Read images and metadata |
| Page program | `02` | Program at most one page at a time |
| Sector erase | `20` | Erase an aligned 4 KiB sector |

The driver splits writes at page boundaries; a raw page program in the model wraps within its page. Programming can only clear bits. Erasing restores bytes to `FF`. Polling has a bounded attempt count, and bus errors propagate to the caller. The model also supports write disable (`04`) for driver tests.

Each transport callback represents one complete chip-select transaction. A board port would implement that callback with its SPI peripheral and supply an appropriate delay and timeout policy. The simulator does not model electrical signals, exact erase times, quad mode, or the device's full protection register set.

## RTOS scheduling

The scheduler is preemptive, with a 1 kHz tick. The receiver runs at priority 1, controller at 2, and storage worker at 3. FreeRTOS also creates its idle task. Each application task has a 2048-word stack; heap allocation uses `heap_4` with a 1 MiB host budget.

The receiver parses bytes incrementally and sends complete packets through a four-entry queue. A full queue blocks it instead of dropping packets. The controller validates the stream and owns update state. Its SPI callback sends a request to a one-entry queue and blocks for a response. The storage worker alone accesses the NOR model during an upload, removing the need for a shared SPI mutex. Busy polling delays the controller by one tick so other tasks can run.

An event group carries stop and worker-completion flags. The controller stops the receiver, sends a storage shutdown request, and waits for both workers before ending the scheduler. JSON output happens afterward because the POSIX port's signal-based scheduling should not preempt tasks inside host stdio locks. Reported counters show received packets, processed packets, SPI commands, and observed queue depth.

`init`, `status`, `boot`, and `confirm` use the same storage and update modules directly without starting the scheduler. The runtime supports one upload per process.

## Packet format

All multibyte integers are little-endian. A frame has a nine-byte header, payload, and four-byte CRC32.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | Magic bytes `55 AA` |
| 2 | 1 | Type: START = 1, DATA = 2, END = 3 |
| 3 | 4 | Sequence number |
| 7 | 2 | Payload length |
| 9 | 0–256 | Payload |
| 9 + length | 4 | CRC32 of header and payload |

START has sequence 0 and a 12-byte payload: version, image size, and image CRC32, each a 32-bit unsigned integer. DATA carries 1–256 bytes. END has no payload. Sequence numbers must increase by exactly one, and START/END each appear once. CRC uses the reflected polynomial `EDB88320`, compatible with Python's `zlib.crc32`.

The parser rejects invalid headers, excessive lengths, bad checksums, and truncated frames. It aborts rather than searching for a later magic value. The controller rejects repeated or missing sequence numbers and data beyond the declared image size. It waits for end-of-input after END before publishing the image, so trailing frames or junk cannot leave an accepted candidate. Failed transfers restart from the beginning; no resume or acknowledgement protocol is implemented. The CLI limits input streams to 4 MiB.

## Flash layout and commits

| Offset | Size | Region |
| --- | --- | --- |
| `000000` | 4 KiB | Metadata bank 0 |
| `001000` | 4 KiB | Metadata bank 1 |
| `002000` | 256 KiB | Image slot 0 |
| `042000` | 256 KiB | Image slot 1 |
| `082000` | Remaining capacity | Unused |

Each metadata bank contains a 52-byte record at its start:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic `FWU1` |
| 4 | 4 | Generation |
| 8 | 16 | Confirmed image: slot, version, size, CRC32 |
| 24 | 16 | Candidate image: slot, version, size, CRC32 |
| 40 | 4 | Phase: stable = 0, ready = 1, trial = 2 |
| 44 | 4 | CRC32 of bytes 0–43 |
| 48 | 4 | Commit marker `CMIT` |

An absent image has slot `FFFFFFFF` and its other fields zero. The journal rejects impossible sizes, slots, phases, and version relationships even if the record CRC is valid.

A metadata change erases the other bank, writes the first 48 bytes, writes the commit marker last, and verifies the record by reading it back. The previous bank remains intact until the replacement is committed. On reopen, the journal chooses the newest valid record using wrap-aware generation ordering. Conflicting records with equal generations or a half-range generation difference fail closed. Two erased record regions mean an empty device; nonblank metadata with no valid record is corruption.

## Update and recovery rules

| Operation | Preconditions | Result |
| --- | --- | --- |
| Begin upload | Stable phase, newer positive version, size 1–256 KiB | Erase the inactive slot's required sectors |
| Write | Active transfer, exact next offset, bytes within declared size | Program the next image bytes |
| Finish | All bytes received and read-back CRC matches | Commit candidate in ready phase |
| Boot ready image | Candidate CRC matches | Persist trial phase, then select candidate |
| Confirm | Trial phase, matching version and valid image CRC | Promote candidate to confirmed, clear candidate |
| Boot after unconfirmed trial | Trial phase | Clear candidate, select verified confirmed image |

A corrupted ready candidate is discarded before selecting the confirmed image. A corrupted confirmed image is never selected. Storage failures invalidate the current update manager until it is reopened, so callers cannot continue from uncertain in-memory state. `status` reads metadata; `boot` and `confirm` also verify the selected image bytes.

The previous confirmed slot is not erased during an upload. An interruption during image programming leaves no new metadata pointing to incomplete bytes. An interruption during metadata replacement leaves the previous record usable, or the complete new record if its commit marker was persisted. Trial state is committed before reporting a trial selection, so a reset after selection triggers rollback.

First-time provisioning has no confirmed fallback. If its first metadata write is torn and leaves no valid record, reopen reports corruption. The simulator does not guess at an image or silently erase this state; a fresh flash file can be initialized for another provisioning attempt.

## Persistence and fault model

The flash file is exactly 8 MiB. Initialization uses exclusive creation and never overwrites an existing path. An exclusive file lock prevents cooperating processes from opening the same device concurrently. Each mutation is written and `fsync`ed before completion is reported.

`--cut-after N --tear-bytes M` selects a 1-based erase/program operation, applies and persists only its first M bytes, then reports power loss. M defaults to zero and is capped at 4096; values beyond that operation's length apply the whole operation. The model rejects subsequent SPI transactions until reset. A new CLI process reopens the resulting bytes and clears transient device state. `--stuck-busy` keeps BUSY asserted until the polling limit expires.

This is deterministic prefix tearing. It does not model every physical flash failure, arbitrary host filesystem corruption, flash wear, or real-time deadlines. CRCs provide accidental-corruption detection, not authenticity. Version checks are update policy, not cryptographic anti-rollback protection. Image execution, signed manifests, key storage, and STM32 boot handoff are outside the implemented scope.

## Verification

There are 43 Unity tests, 22 Python unittest cases, and one runtime smoke test. Parameterized cases cover all data-payload lengths, each truncation/corruption position in representative frames, and failures at every mutating operation in representative updates and metadata transitions. Tests assert image contents and boot selection after recovery, not only return codes.

The C suites cover SPI page splitting, NOR write rules, transport errors, timeouts, input bounds, CRC vectors, slot alternation, version rejection, journal generations, malformed metadata, and interrupted first installation. Python tests exercise the executable across process restarts, malformed uploads, sender compatibility, maximum-size images, rollback, retries, file locking, and persistent torn writes.

CI runs the full Release suite and a separate AddressSanitizer/UndefinedBehaviorSanitizer build of the C suites. Sanitizers exclude the FreeRTOS POSIX runtime; its scheduler has its own signal and thread behavior. These checks cover the defined model's happy paths and reasonable failure cases, not every possible hardware or scheduling failure.
