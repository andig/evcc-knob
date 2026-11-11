# uses pio if installed, uvx otherwise
PIO := $(shell command -v pio || echo uvx platformio)
export HOMEBREW_PREFIX ?= $(shell brew --prefix 2>/dev/null)

.PHONY: run

# desktop simulator against the evcc in include/config.h
run:
	$(PIO) run -e sim -t exec
