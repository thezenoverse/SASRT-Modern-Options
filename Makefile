# Builds ModernOptions.dll (32-bit) for Sonic & All-Stars Racing Transformed with MinGW-w64.
#   make                          -> build/ModernOptions.dll, copied into mod/
#   make install GAME_DIR="..."   -> copies mod/ to "<GAME_DIR>/mods/Modern Options"

CROSS   ?= i686-w64-mingw32-
CC      := $(CROSS)gcc

CFLAGS  := -O2 -Wall -Wextra -std=gnu11 -DUNICODE -D_UNICODE
LDFLAGS := -shared -static-libgcc -Wl,--dynamicbase -Wl,--nxcompat -Wl,--kill-at
LIBS    := -lkernel32 -luser32 -lgdi32 -lshell32

SRC := src/main.c src/menu.c src/strings.c src/video.c src/settings.c src/live.c src/pause.c src/layout.c src/uidump.c src/d3d9hook.c src/gdicapture.c src/filelog.c src/png.c
OBJ := $(patsubst src/%.c,build/%.o,$(SRC))

all: mod/ModernOptions.dll

build:
	mkdir -p build

build/%.o: src/%.c src/common.h src/game.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/ModernOptions.dll: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)
	$(CROSS)strip --strip-unneeded $@

mod/ModernOptions.dll: build/ModernOptions.dll
	cp $< $@

install: mod/ModernOptions.dll
	@test -n "$(GAME_DIR)" || (echo 'Set GAME_DIR to the game folder' && false)
	mkdir -p "$(GAME_DIR)/mods/Modern Options"
	cp -r mod/. "$(GAME_DIR)/mods/Modern Options/"

clean:
	rm -rf build mod/ModernOptions.dll

.PHONY: all install clean
