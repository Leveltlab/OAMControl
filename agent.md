# Agent guidance for OAMControl

The full spec is [implementationdetails.md](implementationdetails.md). Keep this file and the spec in sync; if they disagree, the spec wins and this file gets fixed.

## Project purpose
Raspberry Pi Pico 2 W firmware that generates a timed gate pulse for the output AOM of a Coherent Monaco laser. A stimulus TTL arms the controller; each microscope frame-onset TTL then starts a delay, after which the gate output goes high for a configured duration. The controller disarms itself after a configured number of gate pulses.

## Hardware model
GPIO numbers are RP2350 GPIOs (what the SDK uses), not physical header pins.

| Signal  | GPIO | Pin | Direction | Connects to |
|---------|------|-----|-----------|-------------|
| ARM     | GP2  | 4   | input     | stimulus computer TTL |
| TRIGGER | GP3  | 5   | input     | 2-photon microscope TTL |
| GATE    | GP15 | 20  | output    | Monaco **Gate 1** via 5 V buffer |

- **No status LED:** the device state is read with `GET`. Do not use GPIO23/24/25/29: on the Pico 2 W they are internal to the CYW43 WiFi chip and not on the header.
- **Monaco Gate 1** (manual p. 3-18): high = 3.3–5.0 V, low = 0–0.5 V, 10 kΩ series resistor, HD-BNC connector. Gate 2 is unused ("spare for future use"). The laser is in Gated Mode (`PM=1` / "Enable External Gate") or Burst Mode; the user chooses, the firmware does not depend on it.
- The Pico's 3.3 V output is exactly the minimum high level, so GATE drives Gate 1 through a 5 V buffer (e.g. 74AHCT1G125) with a ~10 kΩ pull-down on the buffer input.
- TTL inputs need series resistors (~1 kΩ) or a buffer: RP2350 inputs are only 5 V tolerant while powered.

## Timing behaviour (current)
- A rising edge on ARM, or the `ARM` command, arms the controller and latches delay, duration and pulse count.
- Each rising edge on TRIGGER (frame onset) while armed starts the delay; after the delay, GATE goes high for the duration, then low. One gate pulse per trigger.
- The controller disarms by itself after the last of `pulse_count` gate pulses has ended.
- An armed run cannot be stopped, restarted or changed: no reset command, ARM edges ignored while armed, `SET_*` and `ARM` rejected while armed.
- Triggers while disarmed or during a delay or gate pulse are ignored. No glitch filtering.
- The behaviour decisions are recorded in the spec (Q1–Q10). Do not change them in code without asking.

## Units
Microseconds everywhere: delay, duration, and all command parameters.

## Configuration interface
Line-based text over USB CDC serial (UART stdio is also enabled):
- `SET_DELAY <us>` → `delay_us=<us>`
- `SET_PULSE <us>` → `pulse_us=<us>`
- `SET_COUNT <n>` → `pulse_count=<n>` (n >= 1, else `error: count must be >= 1`)
- `GET` → `armed=<0|1> delay_us=<us> pulse_us=<us> pulse_count=<n> remaining=<n> gate=<0|1>`
- `ARM` → `armed`
- `SET_*` or `ARM` while armed → `error: armed`

Every command gets exactly one reply line; clients rely on this to match replies to commands. New transports (e.g. UDP) must use the same command names and units.

GUI: [gui/OAMControl.html](gui/OAMControl.html) uses the Web Serial API (Chrome/Edge) and the commands above. If a command or reply format changes, update the GUI in the same change.

## Constraints
- **Core split:** core 1 busy-polls ARM/TRIGGER and times the gate with no interrupts; core 0 handles serial IO. Serial IO must never interrupt or delay core 1. Only core 1 writes the armed state, remaining count and GATE.
- **Timing path target: PIO.** The polling path gives ~1 µs jitter. New timing work moves trigger → delay → gate → duration into a PIO state machine; the CPU only loads parameters into the FIFO.
- No parameter changes while armed.

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
