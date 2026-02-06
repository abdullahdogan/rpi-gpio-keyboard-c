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

#define MAX_MODS 4
#define MAX_EVENTS 64

/*
  libgpiod + uinput GPIO keyboard
  - Edge BOTH (falling/rising)
  - Press is accepted only if LOW duration >= PRESS_MIN_MS
  - Key is emitted on RISING (release) after duration check
  - Filters short glitches when charger is plugged in

  Wiring assumption (active-low):
    Button between GPIO and GND
    Pull-up enabled (internal bias + optional external 4.7k/10k recommended)
*/

typedef struct {
    unsigned int offset;          // gpiochip line offset (BCM on gpiochip0)
    int keycode;                  // main keycode
    int mods[MAX_MODS];           // modifier keys (optional)
    int nmods;
    const char *name;

    long long last_edge_ms;       // debounce for edges
    long long press_start_ms;     // time when falling happened
    int pressed;                  // 0 released, 1 pressed (latched)
} map_t;

/* ===================== USER MAPPING ===================== */
static map_t MAPS[] = {
    {5,  KEY_ENTER, {0}, 0, "GPIO5->ENTER",   0, 0, 0},
    {27, KEY_ESC,   {0}, 0, "GPIO27->ESC",    0, 0, 0},
    {25, KEY_UP,    {0}, 0, "GPIO25->UP",     0, 0, 0},
    {6,  KEY_DOWN,  {0}, 0, "GPIO6->DOWN",    0, 0, 0},
    {3,  KEY_LEFT,  {0}, 0, "GPIO3->LEFT",    0, 0, 0},
    {7,  KEY_RIGHT, {0}, 0, "GPIO7->RIGHT",   0, 0, 0},
};
/* ========================================================= */

static const long long EDGE_DEBOUNCE_MS = 10;   // short edge filter
static const long long PRESS_MIN_MS     = 200;  // accept press if held >= 100ms
static const long long STUCK_RELEASE_MS = 2000; // safety: if no rising arrives

static const char *GPIOCHIP_PATH = "/dev/gpiochip0";

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
    // modifiers down
    for (int i = 0; i < m->nmods; i++) {
        uinput_emit_key(m->mods[i], 1);
    }

    // click main key
    uinput_emit_key(m->keycode, 1);
    uinput_emit_key(m->keycode, 0);

    // modifiers up
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
        perror("UI_SET_EVBIT");
        return -1;
    }

    // enable keybits: main + modifiers
    for (int i = 0; i < (int)(sizeof(MAPS) / sizeof(MAPS[0])); i++) {
        if (ioctl(ufd, UI_SET_KEYBIT, MAPS[i].keycode) < 0) {
            perror("UI_SET_KEYBIT main");
            return -1;
        }
        for (int j = 0; j < MAPS[i].nmods; j++) {
            if (ioctl(ufd, UI_SET_KEYBIT, MAPS[i].mods[j]) < 0) {
                perror("UI_SET_KEYBIT mod");
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
        perror("UI_DEV_SETUP");
        return -1;
    }
    if (ioctl(ufd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
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

    // internal pull-up to reduce floating/glitch
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
    struct gpiod_edge_event_buffer *buf = gpiod_edge_event_buffer_new(MAX_EVENTS);
    if (!buf) {
        fprintf(stderr, "gpiod_edge_event_buffer_new failed\n");
        return 1;
    }

    while (1) {
        // timeout poll to run STUCK_RELEASE safety
        int pr = poll(&pfd, 1, 50);
        long long now = now_ms();

        // safety: if rising never comes, unlock after STUCK_RELEASE_MS
        for (int k = 0; k < (int)(sizeof(MAPS) / sizeof(MAPS[0])); k++) {
            if (MAPS[k].pressed && (now - MAPS[k].press_start_ms > STUCK_RELEASE_MS)) {
                MAPS[k].pressed = 0;
                if (debug) {
                    fprintf(stdout, "STUCK release -> offset %u\n", MAPS[k].offset);
                    fflush(stdout);
                }
            }
        }

        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pr == 0) continue; // timeout

        int rd = gpiod_line_request_read_edge_events(req, buf, MAX_EVENTS);
        if (rd < 0) {
            perror("read_edge_events");
            continue;
        }

        for (int i = 0; i < rd; i++) {
            const struct gpiod_edge_event *ev = gpiod_edge_event_buffer_get_event(buf, i);
            unsigned int off = gpiod_edge_event_get_line_offset(ev);
            enum gpiod_edge_event_type et = gpiod_edge_event_get_event_type(ev);
            long long t = now_ms();

            int idx = map_index_from_offset(off);
            if (idx < 0) continue;

            // edge debounce
            if (t - MAPS[idx].last_edge_ms < EDGE_DEBOUNCE_MS) continue;
            MAPS[idx].last_edge_ms = t;

            if (et == GPIOD_EDGE_EVENT_FALLING_EDGE) {
                if (!MAPS[idx].pressed) {
                    MAPS[idx].pressed = 1;
                    MAPS[idx].press_start_ms = t;
                    if (debug) {
                        fprintf(stdout, "FALL %u start\n", off);
                        fflush(stdout);
                    }
                }
            } else if (et == GPIOD_EDGE_EVENT_RISING_EDGE) {
                if (MAPS[idx].pressed) {
                    long long dur = t - MAPS[idx].press_start_ms;
                    MAPS[idx].pressed = 0;

                    if (dur >= PRESS_MIN_MS) {
                        if (debug) {
                            fprintf(stdout, "RISE %u dur=%lldms -> EMIT %s\n", off, dur, MAPS[idx].name);
                            fflush(stdout);
                        }
                        emit_mapping(&MAPS[idx]);
                    } else {
                        if (debug) {
                            fprintf(stdout, "RISE %u dur=%lldms -> IGNORE\n", off, dur);
                            fflush(stdout);
                        }
                    }
                }
            }
        }
    }

    cleanup(0);
    return 0;
}
