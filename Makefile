CC       ?= gcc
PKG_CONF ?= pkg-config
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

TARGET   := xdynamicisland
TEST_BIN := build/test_runner

PKGS     := x11 xext xrandr cairo pangocairo dbus-1
PKG_CFLAGS := $(shell $(PKG_CONF) --cflags $(PKGS))
PKG_LIBS   := $(shell $(PKG_CONF) --libs $(PKGS))

WARN_FLAGS := -Wall -Wextra -Werror -Wshadow -Wformat=2
BASE_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Iinclude $(WARN_FLAGS)

# Performance first (-O3 -flto), then minimal footprint & binary size (-ffunction-sections, gc-sections)
CFLAGS   ?= -O3 -flto -ffunction-sections -fdata-sections -fomit-frame-pointer -fno-asynchronous-unwind-tables
LDFLAGS  ?= -flto -Wl,--gc-sections -Wl,--as-needed

ALL_CFLAGS := $(BASE_CFLAGS) $(CFLAGS) $(PKG_CFLAGS)
ALL_LIBS   := $(PKG_LIBS) -lm -lpthread -ldl

SRCS := src/main.c \
        src/instance.c \
        src/spring.c \
        src/config.c \
        src/audio_viz.c \
        src/obstacles.c \
        src/notifications.c \
        src/mpv.c \
        src/media.c \
        src/art.c \
        src/ui.c \
        src/settings_ui.c

OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

TEST_SRCS := tests/test_all.c \
             src/instance.c \
             src/spring.c \
             src/config.c \
             src/audio_viz.c \
             src/obstacles.c \
             src/notifications.c \
             src/mpv.c \
             src/media.c

.PHONY: all release test sanitize install uninstall clean size-check

all: $(TARGET)

build:
	@mkdir -p build

HEADERS := $(wildcard include/*.h)

build/%.o: src/%.c $(HEADERS) | build
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) $(OBJS) -o $@ $(ALL_LIBS)

release: $(TARGET)
	strip --strip-all $(TARGET)
	@ls -lh $(TARGET)

test: | build
	$(CC) $(BASE_CFLAGS) -O2 -g $(PKG_CFLAGS) $(TEST_SRCS) -o $(TEST_BIN) $(PKG_LIBS) -lm -lpthread -ldl
	./$(TEST_BIN)

sanitize: | build
	$(CC) $(BASE_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(PKG_CFLAGS) $(TEST_SRCS) -o build/test_san $(PKG_LIBS) -lm -lpthread -ldl
	./build/test_san

size-check: release
	@SIZE=$$(stat -c%s $(TARGET)); \
	echo "Stripped binary size: $$SIZE bytes ($$((SIZE / 1024)) KB)"; \
	if [ "$$SIZE" -gt 358400 ]; then echo "ERROR: binary exceeds 350 KB budget"; exit 1; fi

install: release
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -rf build $(TARGET)
