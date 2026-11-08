# SPDX-License-Identifier: MIT
# Build with the TI/Mitto msp430-gcc in ~/ti; flash with CCS's DSLite (both x86_64, run under Rosetta).
#
#   make                              build the counter for the board's MSP430FR2422
#   make flash                        build, then program and verify through a LaunchPad's eZ-FET, and reset
#   make FLIP=1 flash                 the same picture turned through 180 degrees, if the digits come out upside down
#   make FAST=1 START=9958 flash      test build: count once a second from 99:58, stored screen redrawn every 30 s
#   make FULL_EVERY=10                partial updates between full refreshes (default 5)
#   make preview DIGITS=1234 VBAT=298 TEMP=17   draw a screen on this Mac: build/preview.pbm
#   make preview DIGITS=4207 VBAT=286 STORED=1   the stored screen, stopped at 42:07

MCU     ?= msp430fr2422
TI      ?= $(HOME)/ti
GCC     ?= $(TI)/msp430-gcc/bin
SUPPORT ?= $(TI)/msp430-gcc-support-files/include
DSLITE  ?= /Applications/ti/ccs2101/ccs/ccs_base/DebugServer/bin/DSLite

SRC   = src/main.c src/epd.c src/render.c src/sense.c src/timer.c
BUILD = build/$(MCU)$(if $(FLIP),-flip$(FLIP))$(if $(FAST),-fast)$(if $(START),-from$(START))$(if $(FULL_EVERY),-full$(FULL_EVERY))$(if $(DIAG),-diag)$(if $(LOOP),-loop)

# Test builds. START is hhmm; leading zeros are dropped so C doesn't read it as octal.
# DIAG=1 logs each panel update in backup memory (see src/epd.c; `make readlog`); LOOP=1 refreshes back to back.
DEFS = $(EXTRA) $(if $(FLIP),-DFLIP=$(FLIP)) $(if $(FAST),-DFAST=1) $(if $(FULL_EVERY),-DFULL_EVERY=$(FULL_EVERY)) \
       $(if $(DIAG),-DDIAG=1) $(if $(LOOP),-DLOOP=1) \
       $(if $(START),-DSTART=$(shell echo '$(START)' | sed 's/^0*\([0-9]\)/\1/'))

CC   = $(GCC)/msp430-elf-gcc
SIZE = $(GCC)/msp430-elf-size
CFLAGS  = -mmcu=$(MCU) -I$(SUPPORT) -Os -g -Wall -Wextra -std=gnu11 -ffunction-sections -fdata-sections $(DEFS)
LDFLAGS = -L$(SUPPORT) -Wl,--gc-sections

ELF = $(BUILD)/ocn-epaper.elf

.PHONY: all size flash readlog preview cycle measure clean

all: $(ELF) size compile_flags.txt

# Lets clangd (editor/LSP) resolve msp430.h and the device registers the way msp430-gcc -mmcu does.
compile_flags.txt: Makefile
	printf '%s\n' --target=msp430 -D__$(shell echo $(MCU) | tr a-z A-Z)__ -I$(SUPPORT) -I$(TI)/msp430-gcc/msp430-elf/include -std=gnu11 > $@

$(ELF): $(SRC) $(wildcard src/*.h) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC)

size: $(ELF)
	$(SIZE) $<

# The chip stays in the debugger's session after flashing and won't wake from LPM3.5 until it's reset
# without the debugger: unplug the LaunchPad's USB and plug it back in, or run `make cycle` with the PPK2.
flash: $(ELF)
	$(DSLITE) flash --config=targets/$(MCU).ccxml --verbose --flash --verify --reset 1 $<
	@echo "Flashed. Power-cycle the board without the debugger before it will count or see the magnet."

# Reads the DIAG log (backup memory 0x0660-0x066F). Attaching halts the chip: power-cycle afterwards.
readlog:
	@mkdir -p build
	$(DSLITE) memory --config=targets/$(MCU).ccxml --range=0x0660,8 --size=16 --output=build/bk.bin > build/readlog.txt 2>&1 || (tail -5 build/readlog.txt; false)
	@xxd build/bk.bin

# Host preview of the drawing code, as the panel will show it.
DIGITS ?= 1234
VBAT   ?= 298
TEMP   ?= 17
preview:
	@mkdir -p build
	cc -std=c11 -Wall -Wextra $(if $(FLIP),-DFLIP=$(FLIP)) -o build/preview tools/preview.c src/render.c
	build/preview $(DIGITS) $(VBAT) $(TEMP) $(if $(STORED),stored) > build/preview.pbm
	@echo "build/preview.pbm"

# PPK2 as the supply (see tools/ppk.py): `make cycle` switches it off and on, a clean power-up like fitting the
# cells; `make measure SECONDS=130` power-cycles and measures. VOLTS is the supply voltage.
VOLTS   ?= 3.0
SECONDS ?= 10
PY       = .venv/bin/python

cycle:
	$(PY) tools/ppk.py --volts $(VOLTS) --cycle 1 --seconds 1 --quiet > /dev/null

measure:
	$(PY) tools/ppk.py --volts $(VOLTS) --cycle 1 --seconds $(SECONDS)

clean:
	rm -rf build compile_flags.txt
