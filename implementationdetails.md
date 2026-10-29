OAMControl - Implementation details
===================================

1. Purpose
----------
This application runs on a Raspberry Pi Pico 2 W. It controls the output AOM
of a Coherent Monaco laser with microsecond precision through the laser's
Gate 1 input.

The Pico 2 W receives three TTL signals on three input pins: two from a 2-photon
microscope and one from the stimulus/experimental control computer. 
The stimulus TTL arms the Pico. Once armed, TTLs from the microscope 
(frame and line onsets) determine the onset delay and uptime of a TTL that is
sent to the Gate 1 input of the Monaco laser.
After a frame onset, the Pico waits for a set number of line onsets and then
a delay. It then closes the PMTs, and 1 us later the gate output goes high
for a configured duration. The Monaco laser emits pulses as long as Gate 1 is
high. 1 us after the gate goes low, the PMTs open again.

Scan mode, line count, delay, duration and pulse count are configurable at
run time through a command interface (see section 6).


2. Hardware and pin map:  https://claude.ai/artifact/WCeWWooAtD8uy6jNvMCtzp
-----------------------
Current pin assignment (see OAMControl.c). The firmware uses RP2350 GPIO
numbers (GPn); the physical header pin is listed separately. Pins 3, 8, 13,
18, 23, 28, 33 and 38 are GND.

  Signal    GPIO  Pin  Direction  Source / destination
  ARM       GP2   4    input      stimulus computer TTL
  TRIGGER   GP3   5    input      2-photon microscope TTL (frame onset)
  LINE      GP4   6    input      2-photon microscope TTL (line onset)
  PMT       GP14  19   output     H11706 PMT shutter; high = closed (see 2.3)
  GATE      GP15  20   output     Monaco Gate 1 (via 5 V buffer, see 2.1)

Neighbouring GND pins for the signal returns: pin 3 (next to ARM/TRIGGER),
pin 8 (next to LINE) and pin 18 (next to PMT/GATE).

There is no status LED: the device state (armed, remaining pulses, gate, PMT) is
read with the GET command. Do not use GPIO23, 24, 25 or 29; on the Pico 2 W
they are internal to the CYW43 WiFi chip and not on the header.

2.1 Gate output to the Monaco (Gate 1)
Monaco manual, "Gate 1 and Gate 2" (p. 3-18):
  - Connector: HD-BNC (Amphenol 034-1030); HD-BNC-to-BNC adapters are in the
    laser accessory kit.
  - High input: 3.3 V to 5.0 V. Low input: 0 to 0.5 V.
  - The input has a 10 kohm series resistor (high impedance, ~0.5 mA at 5 V).
  - Gate 1 controls the laser pulse train. Gate 2 is a spare "for future use"
    and does nothing. Use Gate 1.
  - The laser must be in Gated Mode (PM=1, or "Enable External Gate" on the
    Triggering tab of the Coherent GUI) or Burst Mode; see Q9.

A 3.3 V Pico GPIO sits exactly at the minimum high level, with zero margin.
Regulator tolerance, ground offset or cable noise can then give an intermittent
gate. Therefore:
  - Drive Gate 1 through a 5 V buffer, e.g. 74AHCT1G125 (accepts 3.3 V input,
    outputs 5 V, a few ns of delay).
  - Power the buffer from 5 V. VBUS only carries 5 V when the Pico is powered
    over USB; with another power source, use that 5 V supply.
  - Put a pull-down (~10 kohm to GND) on the buffer input so the gate stays
    low during boot, reset, flashing and whenever the pin is not driven.

2.2 TTL inputs (ARM, TRIGGER, LINE)
  - RP2350 inputs only tolerate 5 V while the Pico is powered. A 5 V TTL
    arriving while the Pico is off can damage it. Add a ~1 kohm series
    resistor per input as a minimum, or an input buffer.
  - If ground loops between rigs are a problem, use a fast digital isolator
    (e.g. ADuM series). Do not use optocouplers: they add microseconds of
    variable delay.

2.3 PMT shutter output (Hamamatsu H11706)
  - Shutter input (yellow wire, GND black): 500 ohm, high >= 2.0 V.
    High = shutter closed (PMT off), low = PMT on (normal imaging); the PMT
    is also on when the Pico is off.
  - Shutter limits: closed for 1 us to 10 ms at most, at up to 10 kHz. The
    firmware caps the duration so that duration + 2 us <= 10 ms; the rate
    limit is not reached at microscope frame rates.
  - Driven directly from GP14, no buffer: the firmware sets the pin to
    12 mA drive strength (the 4 mA default is too weak for 500 ohm). Use a
    100 ohm series resistor at the Pico end: ~2.7 V and ~5.5 mA at the
    shutter input, and it damps cable reflections and limits the current
    if the cable is shorted.
  - Pull-down (10 kohm to GND) on GP14, so the shutter stays open (PMT on)
    during boot, reset and flashing.
  - The H11706 closes within 1 us, hence the fixed 1 us guard (Q12).
  - GP14 and GP15 (GATE) are consecutive GPIOs, so the planned PIO program
    can drive both as side-set pins.


3. Signal timing
----------------
Example with line_count = 2:

  ARM      ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
  TRIGGER  _______/‾\____________________________________________
  LINE     __________/‾\___/‾\___________________________________
                           |<- delay ->|1us|<- duration ->|1us|
  PMT      ____________________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\___  (high = closed)
  GATE     ________________________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾\_______
  LASER    ________________________________|‖‖‖‖‖‖‖‖‖‖‖‖‖‖|_______  (pulses at laser rep rate)

With line_count = 0 the delay starts at the TRIGGER edge.

Important: in Gated Mode the laser's own pulse clock keeps running, and the
gate only lets pulses through. The first laser pulse therefore comes up to one
laser period after the gate goes high (1 us at 1 MHz). The number of pulses is
roughly duration x rep rate. Burst Mode is the alternative; see Q9.


4. Arming and triggering behaviour
----------------------------------
Current implementation:
  - A rising edge on the ARM input arms the controller, using the
    line_count, delay, duration and pulse_count set at that moment. There
    is no arm command: only the stimulus computer's ARM TTL can arm the
    controller.
  - Each rising edge on TRIGGER (frame onset), while armed, starts this
    sequence:
      1. wait for line_count rising edges on LINE (default 0: skip);
      2. wait for the delay (default 0), counted from the last of those
         LINE edges, or from the TRIGGER edge when line_count is 0;
      3. PMT high (PMTs closed);
      4. after 1 us, GATE high for the duration, then GATE low;
      5. after 1 us, PMT low (PMTs open).
  - line_count must be smaller than the scan mode's lines per frame:
    0..511 for uni (default), 0..255 for bi.
  - The controller disarms by itself when pulse_count frames (SET_COUNT,
    >= 1, default 1) have been handled. Normally each frame gets a gate
    pulse; a skipped frame (see below) gets none but still counts, so a
    run always ends. GET reports the skipped frames of the run.
  - An armed run cannot be stopped, restarted or changed: ARM edges are
    ignored and SET_* commands are rejected while armed.
  - Triggers while not armed are ignored.
  - Timeout: lines + delay is limited to one frame interval. A trigger
    during steps 1-3 (before GATE goes high) means the next frame has
    started, so the current frame times out: it gets no gate and is
    skipped, and that trigger is ignored.
  - The gate may extend past the next frame onset. A trigger from GATE
    high until the PMTs open (steps 4-5) starts the next frame: its
    LINE edges are counted from then on, and its sequence continues once
    the current one has ended, so its gate comes one frame interval after
    the current one. Only one such trigger is taken per sequence, and only
    if the run has a pulse left.
  - Duration cap: duration + 2 us must be shorter than the frame interval.
    SET_PULSE rejects a longer duration (when a frame period has been
    measured), and at run time each gate is cut to the measured frame
    period - 3 us, in case the frame rate changed after the setting.
  - If a carried-over frame's PMT close time has already passed when the
    current sequence ends, it is skipped rather than gated late. With the
    duration cap this happens only through timing jitter.
  - After the run the user can change parameters and arm again.

Frame and line measurement:
  - Core 1 timestamps every TRIGGER and LINE rising edge, armed or not.
  - frame_us: the interval between the last two TRIGGER edges.
  - line_ns: the mean LINE interval within the last full frame (first to
    last LINE edge, divided by the number of intervals), in ns so that
    line_count x line period stays accurate over 511 lines.
  - frame_lines: the number of LINE edges in the last full frame.
  - When no TRIGGER has arrived for 2 s, the microscope is taken to have
    stopped and all three read 0 (not measured). The first interval after
    such a gap is not used.
  - Without a measured frame period, SET_DELAY and SET_PULSE are not
    checked and the run-time duration cap is off; the timeout still works
    because it is the next TRIGGER edge itself.

Decisions:
  Q1  Arming is edge-based: a rising edge on ARM arms the controller; the
      ARM level afterwards has no effect. ARM edges while armed are
      ignored (no re-arm).
  Q2  One gate pulse per microscope TTL until disarmed. The number of gate
      pulses per arm is set with SET_COUNT; the controller disarms after
      the last one.
  Q3  Only the pulse count disarms: the controller disarms automatically
      after pulse_count frames have been handled (skipped frames count). There is no
      reset or abort command; an armed run cannot be interrupted by the
      user.
  Q4  The microscope TTL is the frame onset, so each gate pulse is
      triggered by a frame onset. The trigger rate is the frame rate.
      lines + delay must be shorter than the frame period, so the gate
      starts within the frame of its trigger; otherwise the frame times
      out at the next trigger. The gate itself may extend past the next
      frame onset, but duration + 2 us is capped below the frame period.
  Q5  A trigger during a sequence never restarts it. Before GATE high it
      times out the current frame and is itself ignored; from GATE high
      until the PMTs open it starts the next frame's sequence, which runs
      after the current one (sequences never overlap) and gates one frame
      interval later.
  Q6  The rising edge of the microscope TTL marks frame onset. No glitch
      filtering for now; to be revisited after verifying the inputs with
      an oscilloscope.
  Q7  No pulse trains: exactly one gate pulse per trigger.
  Q9  Laser mode is the user's choice, set on the laser; the firmware
      does not depend on it. Gated Mode (PM=1): the laser emits pulses
      for as long as the gate is high, so the gate duration sets the
      pulse count (roughly duration x rep rate). Burst Mode: the laser
      emits a fixed 1..1,000,000 pulses when Gate 1 goes high, giving an
      exact count; set the gate duration so the burst fits inside it.
  Q8  No parameter changes while armed. All SET_* commands are rejected
      with "error: armed" until the run has ended. Core 1 also latches
      line_count, delay, duration and count at arm time, so a SET that
      races an ARM edge cannot affect the run.
  Q10 While armed, trigger polling must not be interrupted by serial IO.
      Core 1 polls ARM, TRIGGER and LINE and generates the PMT and gate
      outputs with no interrupts; core 0 handles serial IO and only sets
      parameters.
  Q11 The delay is confined by the frame rate; the number of line inputs
      per frame is confined by the scan mode. Scan mode is a parameter:
      uni (max 512 lines per frame) or bi (max 256 lines per frame).
      line_count must be smaller than that maximum; SET_LINES and
      SET_SCANMODE reject a combination that breaks this (no clipping).
  Q12 The PMTs are closed around every gate pulse: PMT high 1 us before
      GATE goes high, PMT low 1 us after GATE goes low. High = closed.
      The PMTs close within 1 us, so the guard is fixed (not a parameter).
      The 1 us guards are timed in CPU cycles, so each is at least 1 us.
  Q13 The Pico measures the frame period, line period and lines per frame
      from TRIGGER and LINE, and uses them to check SET_DELAY and
      SET_PULSE and to cap the gate at run time. LINE edges do not stop
      during a run; the timeout exists to limit lines + delay to one frame
      interval.

Open questions: none.

5. Timing requirements
----------------------
Units: microseconds for all times (delay and duration); line_count is a
number of LINE edges.

  - Line count range: 0 to max_lines - 1 (511 uni, 255 bi).
  - Delay range: 0 us up to lines + delay + 1 us < frame period.
  - Duration range: 1 us up to duration + 2 us < frame period, and at
    most 9998 us (shutter closed <= 10 ms).
  - Trigger-to-gate latency at delay 0: TBD (target < 1 us).
  - Jitter: target < 50 ns.
  - Duration accuracy: +/- 1 us.

Implementation target: the trigger-to-gate path must not depend on CPU
interrupts. The current code busy-polls the GPIOs on core 1 (no interrupts
on that core) and times delay and duration against the 1 us system timer.
This gives roughly 1 us of jitter, independent of USB or WiFi activity on
core 0. The target design is a PIO state machine: wait for trigger, count the delay, raise
the gate, count the duration, lower the gate. Jitter is then one system clock
(~6.7 ns at 150 MHz). The CPU only loads delay/duration into the PIO FIFO.
The CYW43 uses one PIO block; the RP2350 has three.


6. Control interface
--------------------
Current: line-based text commands over USB CDC serial (and UART stdio):

  SET_SCANMODE <uni|bi>
                   -> scanmode=<uni|bi> max_lines=<512|256>
  SET_LINES <n>    -> line_count=<n>    (LINE edges after TRIGGER, n < max_lines)
  SET_DELAY <us>   -> delay_us=<us>
  SET_PULSE <us>   -> pulse_us=<us>
  SET_COUNT <n>    -> pulse_count=<n>   (frames per arm, n >= 1)
  GET              -> armed=<0|1> delay_us=<us> pulse_us=<us> pulse_count=<n>
                      remaining=<n> gate=<0|1> scanmode=<uni|bi>
                      max_lines=<n> line_count=<n> pmt=<0|1>
                      skipped=<n> frame_us=<us> line_ns=<ns>
                      frame_lines=<n>
                      (pmt=1: PMTs closed; skipped: frames of this run
                      without a gate; frame_us, line_ns, frame_lines:
                      measured, 0 = not measured)

  SET_* while armed         -> error: armed
  SET_COUNT 0               -> error: count must be >= 1
  SET_SCANMODE other        -> error: scanmode must be uni or bi
  SET_SCANMODE bi with line_count >= 256
                            -> error: line_count must be < 256 for bi
  SET_LINES n >= max_lines  -> error: line_count must be < <max_lines>
  SET_DELAY d with line_count x line period + d + 1 >= frame_us
                            -> error: lines + delay must be < frame_us (<us>)
  SET_PULSE p > 9998        -> error: pulse_us must be <= 9998
  SET_PULSE p with p + 2 >= frame_us
                            -> error: pulse_us + 2 must be < frame_us (<us>)

SET_DELAY and SET_PULSE are only checked while a frame period is measured
(the microscope is scanning). SET_DELAY checks against the current
line_count, so send SET_LINES first. To switch to bi with a smaller line
count, send SET_LINES first; to switch to uni with a larger line count,
send SET_SCANMODE first.

USB serial is the primary interface: no network setup, no packet loss, and the
stimulus computer is nearby. UDP over WiFi may be added later as an option; if
so, every command must get a reply (acknowledgement) and use the same command
names and units.

Every command gets exactly one reply line, so a client can match replies to
commands by sending one command at a time.

GUI: gui/OAMControl.html, a single-page web app that talks to the Pico over
USB serial with the Web Serial API (Chrome or Edge, desktop; open the file
directly, no server). It polls GET four times a second, sends SET_*,
locks the parameter fields while armed (arming is only by the ARM input),
shows the measured frame period, line period and lines per frame, and warns
when lines + delay or duration + 2 us is not shorter than the measured frame
period. A MATLAB client can use the same commands with serialport().

Planned additions: STATUS (counters, firmware version), SAVE (store config in
flash), FIRE (manual test pulse, bench only), range checking with an error
reply, and a small Python client for the stimulus computer (e.g. PsychoPy).


7. Safety
---------
  - The gate output must be low by default: at boot, reset, during flashing
    and after a crash (external pull-down, watchdog forces GATE low).
  - The PMTs are closed whenever GATE is high: PMT goes high before GATE
    and low only after GATE is low again.
  - The PMT output is low (PMTs open) at boot, reset and flashing
    (external pull-down).
  - Duration cap in firmware: duration + 2 us < measured frame period and
    <= 10 ms (H11706 shutter limit), checked by SET_PULSE and enforced
    again on every gate.
  - Reject out-of-range or malformed parameter values; never apply a
    partially parsed value.


8. Non-goals
------------
  - No control of other laser parameters (power, rep rate, pulse mode); these
    stay in the Coherent GUI / laser serial interface.
  - No WiFi in the first version.


9. Acceptance tests
-------------------
Measure with an oscilloscope on TRIGGER, LINE, PMT and GATE (and the laser
photodiode or Sync output where available):
  - 1000 triggers at minimum delay, maximum delay and maximum trigger rate.
  - Line counting: with line_count = N, the delay starts at the Nth LINE
    edge after TRIGGER, for N = 0, 1 and max_lines - 1 in both scan modes.
  - PMT guards: PMT rises >= 1 us before GATE rises and falls >= 1 us
    after GATE falls, on every pulse.
  - Measurement: frame_us, line_ns and frame_lines in GET match the
    oscilloscope, and read 0 within 2 s of stopping the microscope.
  - Timeout: with lines + delay longer than a frame (set while the
    microscope is stopped), every frame gets no gate, skipped counts up
    and the run still ends after pulse_count frames.
  - Duration cap: SET_PULSE with duration + 2 us >= frame period is
    rejected; after lowering the frame period during an armed run, the
    gate is cut to frame period - 3 us.
  - Report latency, jitter and duration error against section 5.
  - Verify GATE stays low through power-up, reset and flashing.
  - Verify Gate 1 reads high reliably at the buffer output (> 4.5 V).

