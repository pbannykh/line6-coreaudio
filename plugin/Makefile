# Build the Line 6 CoreAudio HAL plugin.
#
#   make               build Line6.driver (bundle) + ad-hoc signature
#   sudo make install  install into /Library/Audio/Plug-Ins/HAL and restart coreaudiod
#   make uninstall     remove and restart coreaudiod
#   make haltest       build the in-process test harness (see haltest.c)
#   make clean         remove build output
#
# A full Xcode install is not required: the CoreAudio headers ship with the
# Command Line Tools.

BUNDLE  = Line6.driver
EXEC    = Line6HAL
SRC     = Line6HAL.c
SHARED  = ../shared
HALDIR  = /Library/Audio/Plug-Ins/HAL

CC      = clang
CFLAGS  = -Wall -Wextra -O2 -fPIC -I$(SHARED)
LDFLAGS = -bundle -framework CoreFoundation -framework CoreAudio

CONTENTS = $(BUNDLE)/Contents
MACOS    = $(CONTENTS)/MacOS

.PHONY: all clean install uninstall

all: $(BUNDLE)

haltest: haltest.c $(SHARED)/line6_shm.h
	$(CC) -Wall -Wextra -O2 -I$(SHARED) -framework CoreFoundation -framework CoreAudio -o haltest haltest.c

$(BUNDLE): $(SRC) Info.plist $(SHARED)/line6_shm.h
	@rm -rf $(BUNDLE)
	@mkdir -p $(MACOS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(MACOS)/$(EXEC) $(SRC)
	cp Info.plist $(CONTENTS)/Info.plist
	@# ad-hoc signature (fine for personal use; no Developer ID needed)
	codesign --force --sign - --timestamp=none $(BUNDLE)
	@echo "built $(BUNDLE)"

install: all
	@echo "installing into $(HALDIR) (needs sudo)..."
	rm -rf $(HALDIR)/$(BUNDLE)
	cp -R $(BUNDLE) $(HALDIR)/
	@echo "restarting coreaudiod..."
	killall coreaudiod || true

uninstall:
	rm -rf $(HALDIR)/$(BUNDLE)
	killall coreaudiod || true

clean:
	rm -rf $(BUNDLE) haltest
