#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

#define ARM_GPIO 2
#define TRIGGER_GPIO 3
#define GATE_GPIO 15
#define STATUS_GPIO 25

/*
 * Core 1 polls ARM and TRIGGER and generates the gate pulses; it runs no
 * interrupts, so serial IO on core 0 cannot delay or interrupt it.
 * g_armed and g_pulses_remaining are written only by core 1. Core 0 changes
 * the parameters (only while disarmed) and posts ARM requests, which core 1
 * executes. Once armed, a run cannot be stopped or restarted: it disarms by
 * itself after the last gate pulse.
 */
static volatile bool g_armed = false;
static volatile uint32_t g_delay_us = 50000;
static volatile uint32_t g_pulse_us = 5000;
static volatile uint32_t g_pulse_count = 1;  /* gate pulses per arm, >= 1 */
static volatile uint32_t g_pulses_remaining = 0;
static volatile bool g_arm_request = false;

static bool g_arm_prev = false;

/* Parameters latched at arm time; core 1 uses only these while armed. */
static uint32_t g_armed_delay_us;
static uint32_t g_armed_pulse_us;

static void arm(void)
{
    g_armed_delay_us = g_delay_us;
    g_armed_pulse_us = g_pulse_us;
    g_pulses_remaining = g_pulse_count;
    g_armed = true;
    gpio_put(STATUS_GPIO, 1);
}

static void disarm(void)
{
    g_armed = false;
    gpio_put(STATUS_GPIO, 0);
}

/* Arms on an ARM rising edge or an ARM request; ignored while armed. */
static void poll_arm(void)
{
    bool arm_now = gpio_get(ARM_GPIO);
    bool rising = arm_now && !g_arm_prev;
    g_arm_prev = arm_now;

    bool request = g_arm_request;
    if (request) {
        g_arm_request = false;
    }

    if ((rising || request) && !g_armed) {
        arm();
    }
}

static void trigger_core_main(void)
{
    bool trig_prev = gpio_get(TRIGGER_GPIO);
    g_arm_prev = gpio_get(ARM_GPIO);

    while (true) {
        poll_arm();

        bool trig_now = gpio_get(TRIGGER_GPIO);
        bool rising = trig_now && !trig_prev;
        trig_prev = trig_now;
        if (!rising || !g_armed) {
            continue;
        }

        absolute_time_t t_trigger = get_absolute_time();
        busy_wait_until(delayed_by_us(t_trigger, g_armed_delay_us));
        gpio_put(GATE_GPIO, 1);
        busy_wait_us_32(g_armed_pulse_us);
        gpio_put(GATE_GPIO, 0);

        if (--g_pulses_remaining == 0) {
            disarm();
        }

        /* Edges during the delay or pulse are ignored: resample the levels
           so an input that went high meanwhile is not seen as a new edge. */
        trig_prev = gpio_get(TRIGGER_GPIO);
        g_arm_prev = gpio_get(ARM_GPIO);
    }
}

static void handle_command(char *line)
{
    if (line[0] == '\0') {
        return;
    }

    if ((strncmp(line, "SET_", 4) == 0 || strcmp(line, "ARM") == 0) && g_armed) {
        printf("error: armed\n");
        return;
    }

    if (strncmp(line, "SET_DELAY ", 10) == 0) {
        g_delay_us = (uint32_t)strtoul(line + 10, NULL, 10);
        printf("delay_us=%lu\n", (unsigned long)g_delay_us);
        return;
    }

    if (strncmp(line, "SET_PULSE ", 10) == 0) {
        g_pulse_us = (uint32_t)strtoul(line + 10, NULL, 10);
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

    if (strcmp(line, "GET") == 0) {
        printf("armed=%d delay_us=%lu pulse_us=%lu pulse_count=%lu remaining=%lu gate=%d\n",
               g_armed,
               (unsigned long)g_delay_us,
               (unsigned long)g_pulse_us,
               (unsigned long)g_pulse_count,
               (unsigned long)g_pulses_remaining,
               gpio_get(GATE_GPIO));
        return;
    }

    if (strcmp(line, "ARM") == 0) {
        g_arm_request = true;
        printf("armed\n");
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

    gpio_init(GATE_GPIO);
    gpio_set_dir(GATE_GPIO, GPIO_OUT);
    gpio_put(GATE_GPIO, 0);

    gpio_init(STATUS_GPIO);
    gpio_set_dir(STATUS_GPIO, GPIO_OUT);
    gpio_put(STATUS_GPIO, 0);

    multicore_launch_core1(trigger_core_main);

    printf("OAMControl ready\n");
    printf("Commands: SET_DELAY <us>, SET_PULSE <us>, SET_COUNT <n>, GET, ARM\n");

    while (true) {
        process_serial_commands();
        sleep_ms(1);
    }
}
