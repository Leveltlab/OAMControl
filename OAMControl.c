#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

#define ARM_GPIO 2
#define TRIGGER_GPIO 3
#define GATE_GPIO 15
#define STATUS_GPIO 25

static volatile bool g_armed = false;
static volatile bool g_pulse_active = false;
static volatile uint32_t g_delay_us = 50000;
static volatile uint32_t g_pulse_us = 5000;

static void set_gate_state(bool active)
{
    gpio_put(GATE_GPIO, active ? 1 : 0);
    g_pulse_active = active;
}

static int64_t gate_end_callback(alarm_id_t id, void *user_data)
{
    (void)id;
    (void)user_data;
    set_gate_state(false);
    return 0;
}

static int64_t gate_delay_callback(alarm_id_t id, void *user_data)
{
    (void)id;
    (void)user_data;
    set_gate_state(true);
    add_alarm_in_us(g_pulse_us, gate_end_callback, NULL);
    return 0;
}

static void gpio_irq_callback(uint gpio, uint32_t events)
{
    if ((gpio == ARM_GPIO) && (events & GPIO_IRQ_EDGE_RISE)) {
        g_armed = true;
        gpio_put(STATUS_GPIO, 1);
        return;
    }

    if ((gpio == TRIGGER_GPIO) && (events & GPIO_IRQ_EDGE_RISE)) {
        if (!g_armed || g_pulse_active) {
            return;
        }

        g_armed = false;
        gpio_put(STATUS_GPIO, 0);
        add_alarm_in_us(g_delay_us, gate_delay_callback, NULL);
    }
}

static void handle_command(char *line)
{
    if (line[0] == '\0') {
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

    if (strcmp(line, "GET") == 0) {
        printf("armed=%d delay_us=%lu pulse_us=%lu gate=%d\n",
               g_armed,
               (unsigned long)g_delay_us,
               (unsigned long)g_pulse_us,
               gpio_get(GATE_GPIO));
        return;
    }

    if (strcmp(line, "ARM") == 0) {
        g_armed = true;
        gpio_put(STATUS_GPIO, 1);
        printf("armed\n");
        return;
    }

    if (strcmp(line, "RESET") == 0) {
        g_armed = false;
        set_gate_state(false);
        gpio_put(STATUS_GPIO, 0);
        printf("reset\n");
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

    gpio_set_irq_enabled_with_callback(ARM_GPIO, GPIO_IRQ_EDGE_RISE, true, gpio_irq_callback);
    gpio_set_irq_enabled(TRIGGER_GPIO, GPIO_IRQ_EDGE_RISE, true);

    printf("OAMControl ready\n");
    printf("Commands: SET_DELAY <us>, SET_PULSE <us>, GET, ARM, RESET\n");

    while (true) {
        process_serial_commands();
        sleep_ms(1);
    }
}
