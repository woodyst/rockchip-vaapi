CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -fPIC -shared \
           $(shell pkg-config --cflags libva 2>/dev/null) \
           $(shell pkg-config --cflags x11 2>/dev/null) \
           $(shell pkg-config --cflags egl glesv2 2>/dev/null) \
           -I/usr/include/rockchip
LDFLAGS := $(shell pkg-config --libs libva 2>/dev/null) \
           $(shell pkg-config --libs x11 2>/dev/null) \
           $(shell pkg-config --libs egl glesv2 2>/dev/null) \
           -lrockchip_mpp -lpthread -ldl

TARGET  := rockchip_drv_video.so
SRCS    := src/rockchip_drv_video.c src/h264.c
OBJS    := $(SRCS:.c=.o)
INSTALL_DIR := /usr/lib/aarch64-linux-gnu/dri

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

install: $(TARGET)
	sudo install -m 755 $(TARGET) $(INSTALL_DIR)/

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install clean
