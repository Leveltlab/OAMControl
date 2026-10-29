#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"

/* RP2350 GPIO numbers, not physical header pins. */
#define ARM_GPIO 2       /* pin 4 */
#define TRIGGER_GPIO 3   /* pin 5 */
#define LINE_GPIO 4      /* pin 6 */
#define PMT_GPIO 14      /* pin 19, high = PMTs closed */
#define GATE_GPIO 15     /* pin 20 */

/* Maximum line inputs per frame for each scan mode. */
#define UNI_MAX_LINES 512
#define BI_MAX_LINES 256

/* PMTs close this long before GATE goes high and reopen this long after.
   Fixed: the PMTs close within 1 us. */
#define PMT_GUARD_US 1

/* H11706 shutter limit: closed for at most 10 ms, so duration + both guards
   must not exceed this. */
#define PMT_MAX_CLOSED_US 10000
#define MAX_PULSE_US (PMT_MAX_CLOSED_US - 2 * PMT_GUARD_US)

/* No TRIGGER for this long: the microscope has stopped, periods unknown. */
#define FRAME_GAP_US 2000000

/*
 * Core 1 polls ARM, TRIGGER and LINE and generates the PMT and gate pulses;
 * it runs no interrupts, so serial IO on core 0 cannot delay or interrupt it.
 * g_armed, g_pulses_remaining, g_frames_skipped and the measurements are
 * written only by core 1. Core 0 only changes the parameters, and only while
 * disarmed. Arming is done solely by the ARM input. Once armed, a run cannot
 * be stopped or restarted: it disarms by itself after pulse_count frames.
 */
static volatile bool g_armed = false;
static volatile uint32_t g_line_count = 0;   /* LINE edges after TRIGGER, < max_lines() */
static volatile uint32_t g_delay_us = 0;
static volatile uint32_t g_pulse_us = 5000;
static volatile uint32_t g_pulse_count = 1;  /* frames per arm, >= 1 */
static volatile uint32_t g_pulses_remaining = 0;
static volatile uint32_t g_frames_skipped = 0;  /* frames in this run that got no gate */
static volatile bool g_scan_bidir = false;   /* scan mode: false = uni, true = bi */

/* Measured from TRIGGER and LINE edges, armed or not; 0 = not measured. */
static volatile uint32_t g_frame_us = 0;        /* last TRIGGER-to-TRIGGER interval */
static volatile uint32_t g_line_ns = 0;         /* mean LINE interval in that frame */
static volatile uint32_t g_frame_lines = 0;     /* LINE edges in that frame */
static volatile uint32_t g_last_trigger_us = 0; /* time_us_32() of the last TRIGGER edge */

static bool g_arm_prev = false;
static uint32_t g_guard_cycles;              /* PMT_GUARD_US in system clock cycles */

/* Parameters latched at arm time; core 1 uses only these while armed. */
static uint32_t g_armed_line_count;
static uint32_t g_armed_delay_us;
static uint32_t g_armed_pulse_us;

static void arm(void)
{
    g_armed_line_count = g_line_count;
    g_armed_delay_us = g_delay_us;
    g_armed_pulse_us = g_pulse_us;
    g_pulses_remaining = g_pulse_count;
    g_frames_skipped = 0;
    g_armed = true;
}

static void disarm(void)
{
    g_armed = false;
}

/* Arms on an ARM rising edge; ignored while armed. */
static void poll_arm(void)
{
    bool arm_now = gpio_get(ARM_GPIO);
    bool rising = arm_now && !g_arm_prev;
    g_arm_prev = arm_now;

    if (rising && !g_armed) {
        arm();
    }
}

/*
 * One frame's progress towards its gate pulse. Sequence per frame:
 * TRIGGER -> line_count LINE edges -> delay -> PMTs close -> guard
 * -> GATE high for the duration -> GATE low -> guard -> PMTs open.
 */
typedef struct {
    uint32_t lines_left;     /* LINE edges still to count */
    absolute_time_t t_ref;   /* TRIGGER edge, then the last counted LINE edge */
} frame_t;

static bool g_trig_prev = false;
static bool g_line_prev = false;
static absolute_time_t g_trig_time;   /* time of the last TRIGGER rising edge */
static absolute_time_t g_line_time;   /* time of the last LINE rising edge */

/* Line statistics for the frame in progress (since the last TRIGGER). */
static bool g_trigger_seen = false;
static uint32_t g_lines_seen = 0;
static uint32_t g_first_line_us;
static uint32_t g_last_line_us;

static void measure_trigger(uint32_t t)
{
    uint32_t interval = t - g_last_trigger_us;
    bool valid = g_trigger_seen && interval <= FRAME_GAP_US;
    g_frame_us = valid ? interval : 0;
    g_frame_lines = valid ? g_lines_seen : 0;
    g_line_ns = valid && g_lines_seen >= 2
        ? (g_last_line_us - g_first_line_us) * 1000u / (g_lines_seen - 1)
        : 0;
    g_last_trigger_us = t;
    g_trigger_seen = true;
    g_lines_seen = 0;
}

static void measure_line(uint32_t t)
{
    if (g_lines_seen == 0) {
        g_first_line_us = t;
    }
    g_last_line_us = t;
    g_lines_seen++;
}

static bool trigger_rose(void)
{
    bool now = gpio_get(TRIGGER_GPIO);
    bool rose = now && !g_trig_prev;
    g_trig_prev = now;
    if (rose) {
        g_trig_time = get_absolute_time();
        measure_trigger((uint32_t)to_us_since_boot(g_trig_time));
    }
    return rose;
}

static bool line_rose(void)
{
    bool now = gpio_get(LINE_GPIO);
    bool rose = now && !g_line_prev;
    g_line_prev = now;
    if (rose) {
        g_line_time = get_absolute_time();
        measure_line((uint32_t)to_us_since_boot(g_line_time));
    }
    return rose;
}

static void start_frame(frame_t *f, absolute_time_t t_trigger)
{
    f->lines_left = g_armed_line_count;
    f->t_ref = t_trigger;
}

static void count_line(frame_t *f, absolute_time_t now)
{
    if (f->lines_left > 0 && --f->lines_left == 0) {
        f->t_ref = now;
    }
}

static absolute_time_t close_time(const frame_t *f)
{
    return delayed_by_us(f->t_ref, g_armed_delay_us);
}

/*
 * From GATE high until the PMTs open again, a TRIGGER starts the next frame
 * (when the run has a pulse left for it) and LINE edges count towards it.
 * Further TRIGGERs in that time are ignored.
 */
static void track_next_frame(frame_t *next, bool *pending, bool allowed)
{
    bool trig = trigger_rose();
    bool line = line_rose();
    if (line && *pending) {
        count_line(next, g_line_time);
    }
    if (trig && allowed && !*pending) {
        start_frame(next, g_trig_time);
        *pending = true;
    }
}

/* Gate duration capped so that duration + both guards < frame interval and
   the shutter limit is kept. */
static uint32_t capped_pulse_us(void)
{
    uint32_t pulse_us = g_armed_pulse_us < MAX_PULSE_US ? g_armed_pulse_us : MAX_PULSE_US;
    uint32_t frame_us = g_frame_us;
    uint32_t max_us = frame_us > 2 * PMT_GUARD_US + 1 ? frame_us - 2 * PMT_GUARD_US - 1 : 0;
    return frame_us != 0 && pulse_us > max_us ? max_us : pulse_us;
}

/*
 * Runs the sequence for frame f. Returns false if f timed out: a TRIGGER
 * arrived before the gate, so lines + delay took a full frame interval. f
 * then gets no gate and that TRIGGER is ignored. *next_started tells whether
 * a TRIGGER during the gate started the next frame.
 */
static bool run_sequence(frame_t *f, frame_t *next, bool *next_started)
{
    *next_started = false;

    while (f->lines_left > 0) {
        if (trigger_rose()) {
            return false;
        }
        if (line_rose()) {
            count_line(f, g_line_time);
        }
    }
    absolute_time_t t_close = close_time(f);
    while (!time_reached(t_close)) {
        if (trigger_rose()) {
            return false;
        }
        line_rose();
    }

    gpio_put(PMT_GPIO, 1);
    busy_wait_at_least_cycles(g_guard_cycles);
    if (trigger_rose()) {
        gpio_put(PMT_GPIO, 0);
        return false;
    }
    line_rose();
    gpio_put(GATE_GPIO, 1);

    bool allowed = g_pulses_remaining > 1;
    absolute_time_t t_gate_end = delayed_by_us(get_absolute_time(), capped_pulse_us());
    while (!time_reached(t_gate_end)) {
        track_next_frame(next, next_started, allowed);
    }
    gpio_put(GATE_GPIO, 0);
    busy_wait_at_least_cycles(g_guard_cycles);
    gpio_put(PMT_GPIO, 0);

    /* Edges during the guard are picked up here, up to 1 us late. */
    track_next_frame(next, next_started, allowed);
    return true;
}

static void trigger_core_main(void)
{
    g_trig_prev = gpio_get(TRIGGER_GPIO);
    g_line_prev = gpio_get(LINE_GPIO);
    g_arm_prev = gpio_get(ARM_GPIO);

    frame_t frame;
    frame_t next;
    bool pending = false;

    while (true) {
        poll_arm();

        bool carried = pending;
        if (pending) {
            frame = next;
        } else {
            bool trig = trigger_rose();
            line_rose();
            if (!trig || !g_armed) {
                continue;
            }
            start_frame(&frame, g_trig_time);
        }

        /* A carried frame whose PMT close time passed during the previous
           gate is skipped rather than gated late. The duration cap keeps
           this from happening except through timing jitter. */
        bool gated;
        if (carried && frame.lines_left == 0 && time_reached(close_time(&frame))) {
            pending = false;
            gated = false;
        } else {
            gated = run_sequence(&frame, &next, &pending);
        }

        /* Skipped frames count towards pulse_count, so a run always ends. */
        if (!gated) {
            g_frames_skipped++;
        }
        if (--g_pulses_remaining == 0) {
            disarm();
        }

        /* ARM edges during the sequence are ignored: resample the level so
           an ARM that went high meanwhile is not seen as a new edge. */
        g_arm_prev = gpio_get(ARM_GPIO);
    }
}

static uint32_t max_lines(void)
{
    return g_scan_bidir ? BI_MAX_LINES : UNI_MAX_LINES;
}

/* Measured frame period, or 0 if not measured or TRIGGER has stopped. */
static uint32_t measured_frame_us(void)
{
    uint32_t frame_us = g_frame_us;
    if (frame_us == 0 || time_us_32() - g_last_trigger_us > FRAME_GAP_US) {
        return 0;
    }
    return frame_us;
}

static void handle_command(char *line)
{
    if (line[0] == '\0') {
        return;
    }

    if (strncmp(line, "SET_", 4) == 0 && g_armed) {
        printf("error: armed\n");
        return;
    }

    /* SET_DELAY and SET_PULSE are checked against the measured frame period
       when there is one. Lines are estimated as line_count x line period;
       the run-time timeout and cap enforce the limits exactly. */
    if (strncmp(line, "SET_DELAY ", 10) == 0) {
        uint32_t delay = (uint32_t)strtoul(line + 10, NULL, 10);
        uint32_t frame_us = measured_frame_us();
        uint64_t lines_us = frame_us ? (uint64_t)g_line_count * g_line_ns / 1000 : 0;
        if (frame_us && lines_us + delay + PMT_GUARD_US >= frame_us) {
            printf("error: lines + delay must be < frame_us (%lu)\n", (unsigned long)frame_us);
            return;
        }
        g_delay_us = delay;
        printf("delay_us=%lu\n", (unsigned long)g_delay_us);
        return;
    }

    if (strncmp(line, "SET_PULSE ", 10) == 0) {
        uint32_t pulse = (uint32_t)strtoul(line + 10, NULL, 10);
        if (pulse > MAX_PULSE_US) {
            printf("error: pulse_us must be <= %d\n", MAX_PULSE_US);
            return;
        }
        uint32_t frame_us = measured_frame_us();
        if (frame_us && (uint64_t)pulse + 2 * PMT_GUARD_US >= frame_us) {
            printf("error: pulse_us + 2 must be < frame_us (%lu)\n", (unsigned long)frame_us);
            return;
        }
        g_pulse_us = pulse;
        printf("pulse_us=%lu\n", (unsigned long)g_pulse_us);
        return;
    }

    if (strncmp(line, "SET_COUNT ", 10) == 0) {
        uint32_t count = (uint32_t)strtoul(line + 10, NULL, 10);
        if (count == 0) {
            printf("error: count must be >= 1\n");
            return;
        }
        g_pulse_count = count;
        printf("pulse_count=%lu\n", (unsigned long)g_pulse_count);
        return;
    }

    if (strncmp(line, "SET_LINES ", 10) == 0) {
        uint32_t lines = (uint32_t)strtoul(line + 10, NULL, 10);
        if (lines >= max_lines()) {
            printf("error: line_count must be < %lu\n", (unsigned long)max_lines());
            return;
        }
        g_line_count = lines;
        printf("line_count=%lu\n", (unsigned long)g_line_count);
        return;
    }

    if (strncmp(line, "SET_SCANMODE ", 13) == 0) {
        const char *mode = line + 13;
        bool bidir;
        if (strcmp(mode, "uni") == 0) {
            bidir = false;
        } else if (strcmp(mode, "bi") == 0) {
            bidir = true;
        } else {
            printf("error: scanmode must be uni or bi\n");
            return;
        }
        if (bidir && g_line_count >= BI_MAX_LINES) {
            printf("error: line_count must be < %d for bi\n", BI_MAX_LINES);
            return;
        }
        g_scan_bidir = bidir;
        printf("scanmode=%s max_lines=%lu\n",
               g_scan_bidir ? "bi" : "uni", (unsigned long)max_lines());
        return;
    }

    if (strcmp(line, "GET") == 0) {
        uint32_t frame_us = measured_frame_us();
        printf("armed=%d delay_us=%lu pulse_us=%lu pulse_count=%lu remaining=%lu gate=%d"
               " scanmode=%s max_lines=%lu line_count=%lu pmt=%d"
               " skipped=%lu frame_us=%lu line_ns=%lu frame_lines=%lu\n",
               g_armed,
               (unsigned long)g_delay_us,
               (unsigned long)g_pulse_us,
               (unsigned long)g_pulse_count,
               (unsigned long)g_pulses_remaining,
               gpio_get(GATE_GPIO),
               g_scan_bidir ? "bi" : "uni",
               (unsigned long)max_lines(),
               (unsigned long)g_line_count,
               gpio_get(PMT_GPIO),
               (unsigned long)g_frames_skipped,
               (unsigned long)frame_us,
               (unsigned long)(frame_us ? g_line_ns : 0),
               (unsigned long)(frame_us ? g_frame_lines : 0));
        return;
    }

    printf("unknown command: %s\n", line);
}

static void process_serial_commands(void)
{
    static char line[64];
    static size_t idx = 0;

    while (true) {
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) {
            break;
        }

        if (ch == '\n' || ch == '\r') {
            if (idx > 0) {
                line[idx] = '\0';
                handle_command(line);
                idx = 0;
            }
            continue;
        }

        if (idx < sizeof(line) - 1) {
            line[idx++] = (char)ch;
        }
    }
}

int main()
{
    stdio_init_all();

    gpio_init(ARM_GPIO);
    gpio_set_dir(ARM_GPIO, GPIO_IN);
    gpio_pull_down(ARM_GPIO);

    gpio_init(TRIGGER_GPIO);
    gpio_set_dir(TRIGGER_GPIO, GPIO_IN);
    gpio_pull_down(TRIGGER_GPIO);

    gpio_init(LINE_GPIO);
    gpio_set_dir(LINE_GPIO, GPIO_IN);
    gpio_pull_down(LINE_GPIO);

    gpio_init(GATE_GPIO);
    gpio_set_dir(GATE_GPIO, GPIO_OUT);
    gpio_put(GATE_GPIO, 0);

    gpio_init(PMT_GPIO);
    gpio_set_dir(PMT_GPIO, GPIO_OUT);
    gpio_put(PMT_GPIO, 0);
    /* The H11706 shutter input is 500 ohm: ~6 mA, above the 4 mA default. */
    gpio_set_drive_strength(PMT_GPIO, GPIO_DRIVE_STRENGTH_12MA);

    g_guard_cycles = clock_get_hz(clk_sys) / 1000000 * PMT_GUARD_US;

    multicore_launch_core1(trigger_core_main);

    printf("OAMControl ready\n");
    printf("Commands: SET_LINES <n>, SET_DELAY <us>, SET_PULSE <us>, SET_COUNT <n>,"
           " SET_SCANMODE <uni|bi>, GET\n");

    while (true) {
        process_serial_commands();
        sleep_ms(1);
    }
}
