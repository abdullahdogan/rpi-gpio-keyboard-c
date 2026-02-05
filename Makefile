CC=gcc
CFLAGS=-Wall -O2

TARGET=gpio_keyboard

all:
	$(CC) $(CFLAGS) src/gpio_keyboard.c -o $(TARGET)

clean:
	rm -f $(TARGET)