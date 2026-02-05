#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#include <linux/uinput.h>
#include <gpiod.h>

/*
  GPIO -> Keyboard (uinput) using libgpiod (no /sys/class/gpio).
  - Uses BOTH edges and per-line state to prevent double press (bounce).
  - Optional modifier keys (CTRL/ALT/SHIFT) per mapping.

  Wiring recommendation (active-low):
  - Button between GPIO and GND
  - Enable pull-up (internal bias + optional external 10k to 3.3V)
*/

#define MAX_MODS 4

typedef struct {
    unsigned int offset;          // gpiochip0 line offset (BCM)
    int keycode;                  // main key
    int mods[MAX_MODS];           // modifiers (optional)
    int nmods;
    const char *name;

    long long last_ms;            // software debounce timestamp
    int pressed;                  // state: 0 released, 1 pressed (latched)
} map_t;

/* ===================== USER MAPPING ===================== */
/* Change offsets / keys / combos here. Examples:
   - Single key: {17, KEY_ENTER, {0}, 0, "GPIO17->ENTER", 0, 0}
   - Combo: CTRL+L: {17, KEY_L, {KEY_LEFTCTRL}, 1, "GPIO17->CTRL+L", 0, 0}
*/
static map_t MAPS[] = {
    {5, KEY_ENTER, {0}, 0, "GPIO5->ENTER", 0, 0},
    {27, KEY_ESC,   {0}, 0, "GPIO27->ESC",   0, 0},
    {25, KEY_UP,    {0}, 0, "GPIO25->UP",    0, 0},
    {6, KEY_DOWN,  {0}, 0, "GPIO6->DOWN",  0, 0},
    {3, KEY_LEFT,    {0}, 0, "GPIO3->LEFT",    0, 0},
    {7, KEY_RIGHT,  {0}, 0, "GPIO7->RIGHT",  0, 0},
};
/* ========================================================= */

static const char *GPIOCHIP_PATH = "/dev/gpiochip0";

/* Debounce window in ms.
   - Typical mechanical buttons: 20-80ms
   - If you still see double triggers, try 150-300ms
*/
static const long long DEBOUNCE_MS = 50;

static int ufd = -1;
static struct gpiod_chip *chip = NULL;
static struct gpiod_line_request *req = NULL;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void uinput_emit_key(int key, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = key;
    ev.value = value;
    (void)write(ufd, &ev, sizeof(ev));
}

static void uinput_sync(void) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    (void)write(ufd, &ev, sizeof(ev));
}

static void emit_mapping(const map_t *m) {
    // Press modifiers
    for (int i = 0; i < m->nmods; i++) {
        uinput_emit_key(m->mods[i], 1);
    }

    // Click main key
    uinput_emit_key(m->keycode, 1);
    uinput_emit_key(m->keycode, 0);

    // Release modifiers (reverse order)
    for (int i = m->nmods - 1; i >= 0; i--) {
        uinput_emit_key(m->mods[i], 0);
    }

    uinput_sync();
}

static int map_index_from_offset(unsigned int off) {
    for (int i = 0; i < (int)(sizeof(MAPS) / sizeof(MAPS[0])); i++) {
        if (MAPS[i].offset == off) return i;
    }
    return -1;
}

static void cleanup(int sig) {
    (void)sig;

    if (req) {
        gpiod_line_request_release(req);
        req = NULL;
    }
    if (chip) {
        gpiod_chip_close(chip);
        chip = NULL;
    }

    if (ufd >= 0) {
        ioctl(ufd, UI_DEV_DESTROY);
        close(ufd);
        ufd = -1;
    }
    _exit(0);
}

static int setup_uinput(void) {
    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    if (ioctl(ufd, UI_SET_EVBIT, EV_KEY) < 0) {
        perror("ioctl UI_SET_EVBIT");
        return -1;
    }

    // Enable all keys used: main + modifiers
    for (int i = 0; i < (int)(sizeof(MAPS) / sizeof(MAPS[0])); i++) {
        if (ioctl(ufd, UI_SET_KEYBIT, MAPS[i].keycode) < 0) {
            perror("ioctl UI_SET_KEYBIT main");
            return -1;
        }
        for (int j = 0; j < MAPS[i].nmods; j++) {
            if (ioctl(ufd, UI_SET_KEYBIT, MAPS[i].mods[j]) < 0) {
                perror("ioctl UI_SET_KEYBIT mod");
                return -1;
            }
        }
    }

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "GPIO Keyboard");
    us.id.bustype = BUS_USB;
    us.id.vendor  = 0x1234;
    us.id.product = 0x5678;
    us.id.version = 1;

    if (ioctl(ufd, UI_DEV_SETUP, &us) < 0) {
        perror("ioctl UI_DEV_SETUP");
        return -1;
    }
    if (ioctl(ufd, UI_DEV_CREATE) < 0) {
        perror("ioctl UI_DEV_CREATE");
        return -1;
    }

    usleep(300000);
    return 0;
}

static int setup_gpiod_request(void) {
    chip = gpiod_chip_open(GPIOCHIP_PATH);
    if (!chip) {
        perror("gpiod_chip_open");
        return -1;
    }

    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    struct gpiod_line_config *lcfg = gpiod_line_config_new();
    struct gpiod_request_config *rcfg = gpiod_request_config_new();
    if (!settings || !lcfg || !rcfg) {
        fprintf(stderr, "gpiod alloc failed\n");
        return -1;
    }

    gpiod_request_config_set_consumer(rcfg, "gpio-keyboard");

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);

    // Internal pull-up bias: helps against floating inputs
    // If your hardware already has external pull-up, this still OK.
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

    unsigned int offsets[sizeof(MAPS) / sizeof(MAPS[0])];
    for (int i = 0; i < (int)(sizeof(MAPS) / sizeof(MAPS[0])); i++) {
        offsets[i] = MAPS[i].offset;
    }

    if (gpiod_line_config_add_line_settings(
            lcfg,
            offsets,
            (size_t)(sizeof(offsets) / sizeof(offsets[0])),
            settings) < 0) {
        perror("gpiod_line_config_add_line_settings");
        return -1;
    }

    req = gpiod_chip_request_lines(chip, rcfg, lcfg);
    if (!req) {
        perror("gpiod_chip_request_lines");
        return -1;
    }

    gpiod_request_config_free(rcfg);
    gpiod_line_config_free(lcfg);
    gpiod_line_settings_free(settings);

    return 0;
}

int main(int argc, char **argv) {
    bool debug = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) debug = true;
        if (strcmp(argv[i], "--chip") == 0 && i + 1 < argc) {
            GPIOCHIP_PATH = argv[i + 1];
            i++;
        }
    }

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    if (setup_uinput() < 0) {
        fprintf(stderr, "uinput setup failed\n");
        return 1;
    }
    if (setup_gpiod_request() < 0) {
        fprintf(stderr, "gpiod request failed\n");
        return 1;
    }

    int gfd = gpiod_line_request_get_fd(req);
    if (gfd < 0) {
        fprintf(stderr, "gpiod_line_request_get_fd failed\n");
        return 1;
    }

    struct pollfd pfd = { .fd = gfd, .events = POLLIN };
    struct gpiod_edge_event_buffer *buf = gpiod_edge_event_buffer_new(64);
    if (!buf) {
        fprintf(stderr, "gpiod_edge_event_buffer_new failed\n");
        return 1;
    }

    while (1) {
        int pr = poll(&pfd, 1, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        int rd = gpiod_line_request_read_edge_events(req, buf, 64);
        if (rd < 0) {
            perror("read_edge_events");
            continue;
        }

        for (int i = 0; i < rd; i++) {
            const struct gpiod_edge_event *ev = gpiod_edge_event_buffer_get_event(buf, i);
            unsigned int off = gpiod_edge_event_get_line_offset(ev);
            enum gpiod_edge_event_type et = gpiod_edge_event_get_event_type(ev);

            int idx = map_index_from_offset(off);
            if (idx < 0) continue;

            long long t = now_ms();
            if (t - MAPS[idx].last_ms < DEBOUNCE_MS) continue;
            MAPS[idx].last_ms = t;

            if (et == GPIOD_EDGE_EVENT_FALLING_EDGE) {
                // Press (active-low). Only trigger once until release.
                if (!MAPS[idx].pressed) {
                    MAPS[idx].pressed = 1;
                    if (debug) {
                        fprintf(stdout, "FALLING on %u -> %s\n", off, MAPS[idx].name);
                        fflush(stdout);
                    }
                    emit_mapping(&MAPS[idx]);
                } else {
                    if (debug) {
                        fprintf(stdout, "FALLING ignored (already pressed) on %u\n", off);
                        fflush(stdout);
                    }
                }
            } else if (et == GPIOD_EDGE_EVENT_RISING_EDGE) {
                // Release
                if (MAPS[idx].pressed) {
                    MAPS[idx].pressed = 0;
                    if (debug) {
                        fprintf(stdout, "RISING  on %u (release)\n", off);
                        fflush(stdout);
                    }
                } else {
                    if (debug) {
                        fprintf(stdout, "RISING ignored (already released) on %u\n", off);
                        fflush(stdout);
                    }
                }
            }
        }
    }

    cleanup(0);
    return 0;
}
