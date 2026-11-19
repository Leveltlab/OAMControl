# Agent guidance for OAMControl

The full spec is [implementationdetails.md](implementationdetails.md). Keep this file and the spec in sync; if they disagree, the spec wins and this file gets fixed.

## Project purpose
Raspberry Pi Pico 2 W firmware that generates a timed gate pulse for the output AOM of a Coherent Monaco laser. A stimulus TTL arms the controller; each microscope frame-onset TTL then starts a sequence: wait for a number of line-onset TTLs, wait a delay, close the PMTs, and 1 µs later raise the gate for a configured duration; the PMTs reopen 1 µs after the gate ends. The controller disarms itself after a configured number of gate pulses.

## Hardware model
GPIO numbers are RP2350 GPIOs (what the SDK uses), not physical header pins.

| Signal  | GPIO | Pin | Direction | Connects to |
|---------|------|-----|-----------|-------------|
| ARM     | GP2  | 4   | input     | stimulus computer TTL |
| TRIGGER | GP3  | 5   | input     | 2-photon microscope TTL (frame onset) |
| LINE    | GP4  | 6   | input     | 2-photon microscope TTL (line onset) |
| PMT     | GP14 | 19  | output    | H11706 PMT shutter (yellow); high = closed |
| GATE    | GP15 | 20  | output    | Monaco **Gate 1** via 5 V buffer |

- **No status LED:** the device state is read with `GET`. Do not use GPIO23/24/25/29: on the Pico 2 W they are internal to the CYW43 WiFi chip and not on the header.
- **Monaco Gate 1** (manual p. 3-18): high = 3.3–5.0 V, low = 0–0.5 V, 10 kΩ series resistor, HD-BNC connector. Gate 2 is unused ("spare for future use"). The laser is in Gated Mode (`PM=1` / "Enable External Gate") or Burst Mode; the user chooses, the firmware does not depend on it.
- The Pico's 3.3 V output is exactly the minimum high level, so GATE drives Gate 1 through a 5 V buffer (e.g. 74AHCT1G125) with a ~10 kΩ pull-down on the buffer input.
- **H11706 shutter:** 500 Ω input, high ≥ 2.0 V, closed for at most 10 ms. Driven directly from GP14 (no buffer) at 12 mA drive strength through a 100 Ω series resistor, with a 10 kΩ pull-down so the PMT stays on at boot.
- TTL inputs need series resistors (~1 kΩ) or a buffer: RP2350 inputs are only 5 V tolerant while powered.

## Timing behaviour (current)
- A rising edge on the ARM input arms the controller and latches line count, delay, duration and pulse count. Arming is only possible through the ARM input; there is no arm command.
- Each rising edge on TRIGGER (frame onset) while armed starts the sequence: wait for `line_count` LINE rising edges (default 0) → delay (default 0, from the last LINE edge or from TRIGGER if 0) → PMT high → 1 µs → GATE high for the duration → GATE low → 1 µs → PMT low. One gate pulse per trigger.
- Line 1 starts with the frame: its LINE edge coincides with TRIGGER and is never counted (a LINE edge ≤ 2 µs after TRIGGER is taken as line 1). `line_count = N` waits N whole lines, i.e. for the onset of line N + 1; the delay starts there.
- `line_count` must be < the scan mode's lines per frame: 512 for `uni` (default), 256 for `bi`.
- The controller disarms by itself after `pulse_count` gate pulses.
- **Sequence within its frame (Q14):** lines + delay + duration + 2 µs < frame period, so the gate ends and the PMTs reopen before the next TRIGGER. `SET_LINES`, `SET_DELAY` and `SET_PULSE` enforce this when a frame period is measured (lines estimated as `line_count` × `line_ns`). A SET that does not lengthen the sequence is always accepted. No timeout, no carried-over or skipped frames.
- **Shutter cap:** duration ≤ 9998 µs (shutter closed ≤ 10 ms), checked by `SET_PULSE` and applied again at arm.
- **Measurement:** core 1 timestamps every TRIGGER and LINE edge, armed or not, giving `frame_us` (last TRIGGER interval), `line_ns` (mean LINE interval in the last frame) and `frame_lines`. After 2 s without TRIGGER they read 0 (not measured), and `SET_DELAY`/`SET_PULSE` are not checked.
- The PMT guard is a fixed 1 µs (the PMTs close within 1 µs).
- An armed run cannot be stopped, restarted or changed: no reset command, ARM edges ignored while armed, `SET_*` rejected while armed.
- Triggers while disarmed are ignored. No glitch filtering.
- A trigger during a sequence is ignored (measured only; it starts no sequence and is not counted). With the frame check this only happens if the timing was set while the microscope was stopped or the frame rate changed afterwards.
- The behaviour decisions are recorded in the spec (Q1–Q14). Do not change them in code without asking.

## Units
Microseconds everywhere: delay, duration, and all timing command parameters. Line counts are plain integers. The one exception is the measured line period, `line_ns`, in nanoseconds so line_count × line period stays accurate.

## Configuration interface
Line-based text over USB CDC serial (UART stdio is also enabled):
- `SET_SCANMODE <uni|bi>` → `scanmode=<uni|bi> max_lines=<512|256>` (else `error: scanmode must be uni or bi`; `bi` with `line_count >= 256` → `error: line_count must be < 256 for bi`)
- `SET_LINES <n>` → `line_count=<n>` (n < max_lines, else `error: line_count must be < <max_lines>`)
- `SET_DELAY <us>` → `delay_us=<us>`
- `SET_PULSE <us>` → `pulse_us=<us>` (> 9998 → `error: pulse_us must be <= 9998`)
- `SET_LINES`, `SET_DELAY`, `SET_PULSE`: when measured, a value that makes lines × line period + delay + pulse + 2 µs ≥ frame_us (and lengthens the sequence) → `error: lines + delay + pulse + 2 must be < frame_us (<us>)`
- `SET_COUNT <n>` → `pulse_count=<n>` (gate pulses per run, n >= 1, else `error: count must be >= 1`)
- `GET` → `armed=<0|1> delay_us=<us> pulse_us=<us> pulse_count=<n> remaining=<n> gate=<0|1> scanmode=<uni|bi> max_lines=<n> line_count=<n> pmt=<0|1> frame_us=<us> line_ns=<ns> frame_lines=<n>` (`pmt=1` = closed; measured values 0 = not measured)
- `SET_*` while armed → `error: armed`

Every command gets exactly one reply line; clients rely on this to match replies to commands. New transports (e.g. UDP) must use the same command names and units.

GUI: [gui/OAMControl.html](gui/OAMControl.html) uses the Web Serial API (Chrome/Edge) and the commands above. If a command or reply format changes, update the GUI in the same change.

## Constraints
- **Core split:** core 1 busy-polls ARM/TRIGGER/LINE and times PMT and GATE with no interrupts; core 0 handles serial IO and only sets parameters. Serial IO must never interrupt or delay core 1. Only core 1 writes the armed state, remaining count, PMT and GATE.
- **Timing path target: PIO.** The polling path gives ~1 µs jitter. New timing work moves trigger → lines → delay → PMT → gate → duration into a PIO state machine; the CPU only loads parameters into the FIFO.
- No parameter changes while armed.

## Safety
- GATE is low by default: at boot, reset, during flashing and after a crash (external pull-down, watchdog).
- PMTs are closed whenever GATE is high: PMT rises before GATE and falls only after GATE is low. PMT is low (open) at boot (external pull-down).
- Duration cap in firmware: ≤ 9998 µs (shutter closed ≤ 10 ms), checked by `SET_PULSE` and applied again at arm. The frame check keeps each gate inside its own frame.
- Reject malformed or out-of-range values with an error reply; never apply a partially parsed value.

## Build and validation
1. Configure with CMake (`PICO_BOARD=pico2_w`, SDK 2.3.1).
2. Build the firmware from this folder.
3. Flash the generated UF2 to the Pico 2 W.
4. Verify timing on an oscilloscope (TRIGGER, LINE, PMT, GATE) against the acceptance tests in the spec.

## Standards for future work
- Keep the state machine explicit and small.
- Name GPIO roles and timing values clearly; keep units in names (`_us`).
- Add features in small, testable steps.
- Document any change to the external hardware interface in the spec.
