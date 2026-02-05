CC=gcc
CFLAGS=-Wall -O2 $(shell pkg-config --cflags libgpiod)
LDFLAGS=$(shell pkg-config --libs libgpiod)

TARGET=gpio_keyboard

all:
	$(CC) $(CFLAGS) src/gpio_keyboard.c -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
