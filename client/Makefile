CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
TARGET = hid_receiver

all: $(TARGET)

$(TARGET): hid_receiver_linux.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean install
