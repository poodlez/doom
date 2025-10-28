CC ?= gcc
CFLAGS ?= -O2 -pipe -Wall -Wextra
LDFLAGS += -ljpeg -pthread

SRC = src/doom_server.c
OUT = doom_server

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(OUT)

.PHONY: all clean
