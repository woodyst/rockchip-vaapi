CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -fPIC -shared \
           $(shell pkg-config --cflags libva 2>/dev/null) \
           -I/usr/include/rockchip
LDFLAGS := $(shell pkg-config --libs libva 2>/dev/null) \
           -lrockchip_mpp -lpthread -ldl

# Optional: use the RK3588 RGA 2D blitter for the plane copy when librga is
# available.  Falls back to memcpy when it is not.
RGA_LIBS := $(shell pkg-config --libs librga 2>/dev/null)
ifneq ($(RGA_LIBS),)
CFLAGS  += -DHAVE_RGA
LDFLAGS += $(RGA_LIBS)
endif

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
