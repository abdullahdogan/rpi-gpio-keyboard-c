#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <string.h>
#include <poll.h>
#include <signal.h>

#define GPIO_BASE "/sys/class/gpio"

typedef struct {
    int gpio;
    int keycode;
    int fd;
} gpio_key_t;

gpio_key_t keys[] = {
    {17, KEY_ENTER, -1},
    {27, KEY_ESC,   -1},
    {22, KEY_UP,    -1},
    {23, KEY_DOWN,  -1},
};

int uinput_fd;

void export_gpio(int gpio)
{
    int fd = open(GPIO_BASE "/export", O_WRONLY);
    if (fd >= 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", gpio);
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

void setup_gpio(gpio_key_t *k)
{
    char path[128];

    export_gpio(k->gpio);

    snprintf(path, sizeof(path),
             GPIO_BASE "/gpio%d/direction", k->gpio);
    int fd = open(path, O_WRONLY);
    write(fd, "in", 2);
    close(fd);

    snprintf(path, sizeof(path),
             GPIO_BASE "/gpio%d/edge", k->gpio);
    fd = open(path, O_WRONLY);
    write(fd, "falling", 7);
    close(fd);

    snprintf(path, sizeof(path),
             GPIO_BASE "/gpio%d/value", k->gpio);
    k->fd = open(path, O_RDONLY | O_NONBLOCK);
}

void emit_key(int key)
{
    struct input_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = key;
    ev.value = 1;
    write(uinput_fd, &ev, sizeof(ev));

    ev.value = 0;
    write(uinput_fd, &ev, sizeof(ev));

    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(uinput_fd, &ev, sizeof(ev));
}

void cleanup(int sig)
{
    close(uinput_fd);
    exit(0);
}

int main(void)
{
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    for (int i = 0; i < 4; i++) {
        ioctl(uinput_fd, UI_SET_KEYBIT, keys[i].keycode);
    }

    struct uinput_setup usetup = {0};
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "GPIO Keyboard");
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5678;
    usetup.id.version = 1;

    ioctl(uinput_fd, UI_DEV_SETUP, &usetup);
    ioctl(uinput_fd, UI_DEV_CREATE);

    sleep(1);

    struct pollfd fds[4];

    for (int i = 0; i < 4; i++) {
        setup_gpio(&keys[i]);
        fds[i].fd = keys[i].fd;
        fds[i].events = POLLPRI;
    }

    char buf[8];

    while (1) {
        poll(fds, 4, -1);
        for (int i = 0; i < 4; i++) {
            if (fds[i].revents & POLLPRI) {
                lseek(fds[i].fd, 0, SEEK_SET);
                read(fds[i].fd, buf, sizeof(buf));
                emit_key(keys[i].keycode);
            }
        }
    }
}
