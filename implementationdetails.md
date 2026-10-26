OAMControl - Implementation details
===================================

1. Purpose
----------
This application runs on a Raspberry Pi Pico 2 W. It controls the output AOM
(OAM) of a Coherent Monaco laser with microsecond precision through the laser's
Gate 1 input.

The Pico 2 W receives two TTL signals on two input pins: one from the 2-photon
microscope and one from the stimulus computer. The stimulus TTL arms the Pico.
Once armed, a TTL from the microscope starts a timer with a configured delay.
When the delay expires, the gate output goes high for a configured duration.
The gate output is connected to Gate 1 on the Monaco, which emits laser pulses
for as long as Gate 1 is high.

Delay and duration are configurable at run time through a command interface
(see section 6).


2. Hardware and pin map
-----------------------
Current pin assignment (see OAMControl.c). The firmware uses RP2350 GPIO
numbers (GPn); the physical header pin is listed separately. Pins 3, 8, 13,
18, 23, 28, 33 and 38 are GND.

  Signal    GPIO  Pin  Direction  Source / destination
  ARM       GP2   4    input      stimulus computer TTL
  TRIGGER   GP3   5    input      2-photon microscope TTL
  GATE      GP15  20   output     Monaco Gate 1 (via 5 V buffer, see 2.1)

Neighbouring GND pins for the signal returns: pin 3 (next to ARM/TRIGGER)
and pin 18 (next to GATE).

There is no status LED: the device state (armed, remaining pulses, gate) is
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

2.2 TTL inputs (ARM, TRIGGER)
  - RP2350 inputs only tolerate 5 V while the Pico is powered. A 5 V TTL
    arriving while the Pico is off can damage it. Add a ~1 kohm series
    resistor per input as a minimum, or an input buffer.
  - If ground loops between rigs are a problem, use a fast digital isolator
    (e.g. ADuM series). Do not use optocouplers: they add microseconds of
    variable delay.


3. Signal timing
----------------

  ARM      ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
  TRIGGER  _________/‾‾\_______________________________
                    |<-- delay -->|<-- duration -->|
  GATE     _______________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\___
  LASER    _______________________|‖‖‖‖‖‖‖‖‖‖‖‖‖‖‖‖|___  (pulses at laser rep rate)

Important: in Gated Mode the laser's own pulse clock keeps running, and the
gate only lets pulses through. The first laser pulse therefore comes up to one
laser period after the gate goes high (1 us at 1 MHz). The number of pulses is
roughly duration x rep rate. Burst Mode is the alternative; see Q9.


4. Arming and triggering behaviour
----------------------------------
Current implementation:
  - A rising edge on ARM (or the ARM command) arms the controller, using
    the delay, duration and pulse_count set at that moment.
  - Each rising edge on TRIGGER, while armed, starts the delay and then one
    gate pulse.
  - The controller disarms by itself when the last of pulse_count gate
    pulses (SET_COUNT, >= 1, default 1) has ended.
  - An armed run cannot be stopped, restarted or changed: ARM edges, the
    ARM command and SET_* commands are ignored or rejected while armed.
  - Triggers while not armed, or during the delay or gate pulse of a
    previous trigger, are ignored.
  - After the run the user can change parameters and arm again.

Decisions:
  Q1  Arming is edge-based: a rising edge on ARM arms the controller; the
      ARM level afterwards has no effect. ARM edges while armed are
      ignored (no re-arm).
  Q2  One gate pulse per microscope TTL until disarmed. The number of gate
      pulses per arm is set with SET_COUNT; the controller disarms after
      the last one.
  Q3  Only the pulse count disarms: the controller disarms automatically
      after the last of pulse_count gate pulses has ended. There is no
      reset or abort command; an armed run cannot be interrupted by the
      user.
  Q4  The microscope TTL is the frame onset, so each gate pulse is
      triggered by a frame onset. The trigger rate is the frame rate.
      delay + duration must be shorter than the frame period, otherwise
      the next frame onset is ignored and that frame gets no pulse.
  Q5  A new trigger during the delay or the pulse is ignored (no restart).
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
  Q8  No parameter changes while armed. SET_DELAY, SET_PULSE, SET_COUNT
      and ARM are rejected with "error: armed" until the run has ended.
      Core 1 also latches delay, duration and count at arm time, so a SET
      that races an ARM edge cannot affect the run.
  Q10 While armed, trigger polling must not be interrupted by serial IO.
      Core 1 polls ARM and TRIGGER and generates the gate with no
      interrupts; core 0 handles serial IO and only posts parameters and
      ARM requests to core 1.

Open questions: none.

5. Timing requirements
----------------------
Units: microseconds everywhere (delay and duration).

  - Delay range: 0 us to TBD (proposed 10 s).
  - Duration range: 1 us to TBD (proposed 10 s), with a safety cap.
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

  SET_DELAY <us>   -> delay_us=<us>
  SET_PULSE <us>   -> pulse_us=<us>
  SET_COUNT <n>    -> pulse_count=<n>   (gate pulses per arm, n >= 1)
  GET              -> armed=<0|1> delay_us=<us> pulse_us=<us> pulse_count=<n>
                      remaining=<n> gate=<0|1>
  ARM              -> armed

  SET_* and ARM while armed -> error: armed
  SET_COUNT 0               -> error: count must be >= 1

USB serial is the primary interface: no network setup, no packet loss, and the
stimulus computer is nearby. UDP over WiFi may be added later as an option; if
so, every command must get a reply (acknowledgement) and use the same command
names and units.

Every command gets exactly one reply line, so a client can match replies to
commands by sending one command at a time.

GUI: gui/OAMControl.html, a single-page web app that talks to the Pico over
USB serial with the Web Serial API (Chrome or Edge, desktop; open the file
directly, no server). It polls GET four times a second, sends SET_* and ARM,
locks the parameter fields while armed, and warns when delay + duration is
longer than the frame period. A MATLAB client can use the same commands with
serialport().

Planned additions: STATUS (counters, firmware version), SAVE (store config in
flash), FIRE (manual test pulse, bench only), range checking with an error
reply, and a small Python client for the stimulus computer (e.g. PsychoPy).


7. Safety
---------
  - The gate output must be low by default: at boot, reset, during flashing
    and after a crash (external pull-down, watchdog forces GATE low).
  - Enforce a maximum duration cap in firmware.
  - Reject out-of-range or malformed parameter values; never apply a
    partially parsed value.


8. Non-goals
------------
  - No control of other laser parameters (power, rep rate, pulse mode); these
    stay in the Coherent GUI / laser serial interface.
  - No WiFi in the first version.


9. Acceptance tests
-------------------
Measure with an oscilloscope on TRIGGER and GATE (and the laser photodiode
or Sync output where available):
  - 1000 triggers at minimum delay, maximum delay and maximum trigger rate.
  - Report latency, jitter and duration error against section 5.
  - Verify GATE stays low through power-up, reset and flashing.
  - Verify Gate 1 reads high reliably at the buffer output (> 4.5 V).

