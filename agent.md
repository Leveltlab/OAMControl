# Agent guidance for OAMControl

The full spec is [implementationdetails.txt](implementationdetails.txt). Keep this file and the spec in sync; if they disagree, the spec wins and this file gets fixed.

## Project purpose
Raspberry Pi Pico 2 W firmware that generates a timed gate pulse for the output AOM of a Coherent Monaco laser. A stimulus TTL arms the controller; the next microscope TTL starts a delay; after the delay the gate output goes high for a configured duration.

## Hardware model
| Signal  | GPIO | Direction | Connects to |
|---------|------|-----------|-------------|
| ARM     | 2    | input     | stimulus computer TTL |
| TRIGGER | 3    | input     | 2-photon microscope TTL |
| GATE    | 15   | output    | Monaco **Gate 1** via 5 V buffer |
| STATUS  | 25   | output    | status indicator (**must move**, see below) |

- **GPIO25 conflict:** on the Pico 2 W, GPIO23/24/25/29 belong to the CYW43 WiFi chip. Do not use them. STATUS must move to a free GPIO with an external LED.
- **Monaco Gate 1** (manual p. 3-18): high = 3.3–5.0 V, low = 0–0.5 V, 10 kΩ series resistor, HD-BNC connector. Gate 2 is unused ("spare for future use"). The laser must be in Gated Mode (`PM=1` / "Enable External Gate").
- The Pico's 3.3 V output is exactly the minimum high level, so GATE drives Gate 1 through a 5 V buffer (e.g. 74AHCT1G125) with a ~10 kΩ pull-down on the buffer input.
- TTL inputs need series resistors (~1 kΩ) or a buffer: RP2350 inputs are only 5 V tolerant while powered.

## Timing behaviour (current)
- A rising edge on ARM, or the `ARM` command, arms the controller.
- The next rising edge on TRIGGER while armed starts the delay and disarms (one pulse per arm).
- After the delay, GATE goes high for the pulse duration, then low.
- Triggers while disarmed or while the gate is high are ignored.
- Open behaviour questions are listed in the spec (Q1–Q9). Do not decide them in code; ask.

## Units
Microseconds everywhere: delay, duration, and all command parameters.

## Configuration interface
Line-based text over USB CDC serial (UART stdio is also enabled):
- `SET_DELAY <us>` → `delay_us=<us>`
- `SET_PULSE <us>` → `pulse_us=<us>`
- `GET` → `armed=<0|1> delay_us=<us> pulse_us=<us> gate=<0|1>`
- `ARM` → `armed`
- `RESET` → `reset`

Every command gets a reply. New transports (e.g. UDP) must use the same command names and units.

## Constraints
- **Timing path target: PIO.** The current GPIO-interrupt + `add_alarm_in_us()` path gives ~1–5 µs jitter, worse under WiFi/USB load. New timing work moves trigger → delay → gate → duration into a PIO state machine; the CPU only loads parameters into the FIFO.
- Until then: keep the main loop non-blocking, and no blocking I/O or `printf` inside IRQ or alarm callbacks.
- Parameter changes must never take effect mid-pulse.

## Safety
- GATE is low by default: at boot, reset, during flashing and after a crash (external pull-down, watchdog).
- Enforce a maximum duration cap in firmware.
- Reject malformed or out-of-range values with an error reply; never apply a partially parsed value.

## Build and validation
1. Configure with CMake (`PICO_BOARD=pico2_w`, SDK 2.3.1).
2. Build the firmware from this folder.
3. Flash the generated UF2 to the Pico 2 W.
4. Verify timing on an oscilloscope (TRIGGER vs GATE) against the acceptance tests in the spec.

## Standards for future work
- Keep the state machine explicit and small.
- Name GPIO roles and timing values clearly; keep units in names (`_us`).
- Add features in small, testable steps.
- Document any change to the external hardware interface in the spec.
