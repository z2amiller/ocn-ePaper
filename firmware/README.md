# ocn-ePaper firmware

Firmware for the ocn-ePaper board's MSP430FR2422 and its Good Display GDEY0213B74 e-paper panel. It counts
minutes from when the magnet comes off the board and shows HH:MM in large seven-segment digits, freezing at
99:59, with a status line under them: a battery gauge and voltage on the left, the temperature on the right.
Putting the magnet back stops the count and switches to the stored screen, which shows the stopped time.

Adapted from the [ocn-LCD](https://github.com/z2amiller/ocn-LCD) firmware, which does the same on a segment
LCD. The build is about 4.7 KB; the FR2422 has 7,296 bytes of FRAM and 2 KB of RAM.

Status: ported to the FR2422 board (revision 0.3) on 2026-11-08. It builds, but hasn't run on that board yet.
The FR2111 boards (revisions 0.1 and 0.2), where the magnet switches the power, run the firmware as of commit
54b0ff0.

## Measured on the first board

These are from revision 0.1 (FR2111, a CR123A, the magnet switching the power), with a Nordic PPK2 supplying
3.0 V in place of the cell. The panel and its booster are the same on revision 0.3, so the updates should cost
about the same.

| | Charge | Time | Peak |
|---|---|---|---|
| Partial update (each minute) | 1.04 mC | 0.8 s | 58 mA |
| Full refresh (every sixth update) | 3.25 mC | 2.3 s | 59 mA |
| Power-up, including the crystal start and a full refresh | 3.44 mC | 2.4 s | 59 mA |
| Asleep between updates | about 0.4 µA | | |

That averages about 24 µA while counting, over the six-minute cycle of five partial updates and a full
refresh. The updates are nearly all of it: one partial update costs as much as about 40 minutes asleep.

Stored, revision 0.3 should draw about 0.7-0.9 µA: 0.4-0.6 µA asleep (0.4 measured on the FR2111; the
FR2422's datasheet gives 0.63 µA typical for LPM3.5 with the crystal and SVS), plus 0.3 µA through R3 while
the magnet holds WAKE high. The weekly redraw of the stored screen adds about 0.01 µA.

## How it works

- The chip sleeps in LPM3.5, where only the RTC, the 32-byte backup memory and the port interrupts keep
  running. Every wake is a reset: `main()` runs again. The count and the mode live in backup memory, since
  RAM doesn't survive.
- **The magnet** closes the reed switch, which pulls P1.0 (WAKE) high; R3 pulls it low otherwise. The chip
  stays powered either way. Before each sleep it arms Port 1 for the edge that would change the mode: rising
  while counting, falling while stored. Every wake, whatever caused it, compares the pin with the mode, so a
  missed or bouncing edge can't leave them out of step. A change counts once the pin reads the same twice,
  20 ms apart.
- **Counting** (magnet off). The RTC, clocked from the 32.768 kHz crystal, wakes the chip once a minute. Each
  wake wakes the panel with a hardware reset, sends it the old and the new count, and starts a partial update
  (about half a second at ~3.5 mA). After every five partial updates comes a full refresh (about 2.3 s; the
  panel flashes black and white), which clears the ghosting that partial updates leave. Then the panel goes
  back into deep sleep, where it draws about 1 µA. 99:59 is drawn with a full refresh, then the RTC stops.
- **Stored** (magnet on). The count stops where it is, including any minute the RTC counted that the panel
  hadn't shown yet, and the stored screen shows it with a full refresh. The RTC then wakes the chip every
  2,048 s (34 minutes, the longest it can), and every 295th wake, about weekly, the stored screen is drawn
  again. Good Display asks for a full refresh about once a week on a panel that keeps showing the same
  picture.
- **Taking the magnet off** starts a new count from 00:00 with a full refresh.
- **Power-up** (fitting the cells, or the programmer's supply returning), or any reset other than an LPM3.5
  wake, starts afresh: from 00:00 counting if the magnet is off, or the stored screen at 00:00 if it's on.
- While the panel works, the chip waits in LPM3 for BUSY to fall, with Timer_A0 as a timeout, so a missing or
  stuck panel costs a few seconds per update, not flat cells.
- The supply supervisor (SVSH) stays on in LPM3.5, at about 0.24 µA. If the cells ever sag below ~1.8 V, it
  resets the chip cleanly. Without it, the brownout reset is only guaranteed once the supply falls below 0.1 V,
  and the RTC and backup memory could run on below their minimum voltage.
- The panel sleeps in its deep-sleep mode 2, 1 µA typical, which doesn't keep the panel's RAM. Nothing
  needs it to: every update writes both RAMs.
- The RST pin uses the chip's internal pull-up, which the firmware turns on explicitly, so R1 and C11 can be
  left off the board.

**The status line.** `sense.c` uses the ADC and the chip's internal 1.5 V reference, as TI's FR211x examples
do (the FR2422 has the same internal channels and calibration addresses):
- **Battery.** The reference measured against the supply gives the supply: 1.5 V × 1023 / reading, averaged
  over eight readings 1 ms apart. It is read 150 ms into each update, while the panel's booster loads the
  cells, and shown at the next update; at power-up it is read before the first drawing. Under that load the
  reading follows the cells' internal resistance, which climbs with age and cold, so it warns before an update
  could pull the supply under the panel's 2.2 V minimum. The gauge shows four bars from 2.80 V, three from
  2.65, two from 2.50, one from 2.40, and none below: replace the cells. The reference is good to a few
  percent, so treat it as a warning, not a fuel gauge.
- **Temperature.** The chip's own sensor, read at each wake and scaled between TI's factory calibration
  readings at 30 and 85 °C (stored in the chip). The chip sleeps almost all the time, so it sits at the
  temperature around it. On the first board it read 23 °C from the first minute on, in a room at about
  22 °C, but 26 °C on the power-up screen, every time; the cause isn't pinned down (see `sense.c`).

The status line also settles which way up the panel is, which 00:00 alone doesn't. The stored screen reads
portrait, with the MAGNET DISABLE end at the bottom: the stopped time across the top, a placeholder for a
graphic, and the battery gauge and voltage.

**Drawing without a frame buffer.** The panel's frame is 250 × 122 pixels, 3.8 KB, more than the FR2422's
2 KB of RAM. `render.c` computes each 16-byte RAM row as it is sent, straight from the digits' geometry; the
only picture data is a 75-byte 5 × 7 font for the status line, drawn at twice size. The firmware divides with
its own small loops rather than the compiler's library routines, which saves about 400 bytes.

**Telling a wake from a power-up.** As on ocn-LCD: `SYSRSTIV` keeps every reset cause since it was last
emptied, so the code empties it at every start-up and counts a start-up as a wake only when the LPM3.5 wake
is its one cause.

**The panel.** The command sequence follows GxEPD2's driver for this panel (`GxEPD2_213_B74`) and Good
Display's demo code. Both panel RAMs are written on every update (the previous count, then the new one), so a
partial update doesn't depend on anything the panel kept through deep sleep. SPI runs at 4 MHz from an 8 MHz
clock locked to the crystal, on eUSCI_A0's default pins, transmit only.

## Source

- `src/main.c`: the count, the magnet, the two modes, clocks, pins and LPM3.5.
- `src/board.h`: which pin is which.
- `src/epd.c`, `src/epd.h`: the SSD1680 driver: SPI, reset, the update sequence, the BUSY wait and deep sleep.
- `src/render.c`, `src/render.h`: the two screens, one RAM row at a time. Plain C, so it also builds on the
  Mac for the preview.
- `src/sense.c`, `src/sense.h`: the supply voltage and the temperature from the ADC.
- `src/timer.c`, `src/timer.h`: Timer_A0, for waits and timeouts in LPM3.
- `tools/preview.c`: `make preview` draws the panel on the Mac as `build/preview.pbm`.
- `tools/ppk.py`: drives a Nordic PPK2 as the supply and measures the current (from ocn-LCD).
- `targets/msp430fr2422.ccxml`: the DSLite target config for a LaunchPad's eZ-FET programmer.

## Toolchain (macOS)

The same as ocn-LCD's:
- **Compiler:** TI/Mitto msp430-gcc 9.3.1.11 in `~/ti/msp430-gcc` (x86_64, under Rosetta 2).
- **Device headers and linker scripts:** TI's msp430-gcc support files in `~/ti/msp430-gcc-support-files/include`.
- **Flasher:** DSLite from Code Composer Studio, at `/Applications/ti/ccs2101/ccs/ccs_base/DebugServer/bin/DSLite`.
- **PPK2 (optional):** `uv venv .venv && uv pip install --python .venv/bin/python ppk2-api numpy`, in this folder.

Other locations work too: set `TI=`, `GCC=`, `SUPPORT=` or `DSLITE=` on the `make` command line.

## Use

Run these from this folder, with the board wired to a LaunchPad as in the board's
[README](../README.md#programming) (cells out).

```
make                              # build
make flash                        # build, program, verify, hard reset
make FLIP=1 flash                 # the picture turned through 180 degrees
make FAST=1 START=9958 flash      # test build: one count a second from 99:58; stored screen redrawn every 30 s
make FULL_EVERY=10 flash          # ten partial updates between full refreshes instead of five
make preview DIGITS=1234 VBAT=298 TEMP=17   # draw the counting screen on the Mac: build/preview.pbm
make preview DIGITS=4207 VBAT=286 STORED=1  # the stored screen, stopped at 42:07
make DIAG=1 flash                 # log each panel update in backup memory...
make DIAG=1 readlog               # ...and read it back (halts the chip: power-cycle afterwards)
make DIAG=1 LOOP=1 flash          # full refreshes back to back, for metering the booster
make cycle                        # PPK2 power off and on: a clean power-up, like fitting the cells
make measure SECONDS=130          # power-cycle, then measure current for 130 s
```

Each variant builds into its own folder under `build/`; use `make -B` after changing `EXTRA`.

- **After flashing, power-cycle the board.** The panel shows its first screen straight away, but the chip
  stays in the debugger's session after DSLite finishes, and in that state it doesn't wake from LPM3.5: it
  won't count or see the magnet. Unplug the LaunchPad's USB and plug it back in (or `make cycle` with the
  PPK2 as the supply).
- **The magnet.** Bring it to the MAGNET DISABLE end: the count stops within a moment and the stored screen
  comes up. Take it away and a new count starts from 00:00. `FAST=1` shows the stored screen's weekly redraw
  every 30 s instead.
- **Which way up.** With the board read with the MAGNET DISABLE end on the right, the digits should read
  upright. Nothing on the panel's drawing says which end its first RAM row is, so if they come out upside
  down, reflash with `FLIP=1`. The picture is never mirrored: the RAM order is the one GxEPD2 uses for this
  panel.
- **Without the panel** the firmware still runs: each update waits out its timeouts (about 10 s for a full
  refresh, 3 s for a partial update) and carries on.
- **00:00 that never counts, even after a power cycle,** means the crystal (Y1) didn't start: check its
  joints and C9/C10. The firmware gives the crystal about 3 s, then draws its first screen anyway and sleeps
  rather than spin and flatten the cells. The magnet still switches screens, but nothing counts.
- **A blank panel.** Flash `DIAG=1`, power-cycle, then `make DIAG=1 readlog`. Bytes 8 onwards say whether
  BUSY rose after the update command and how long it stayed high (see `src/epd.c`); a working panel holds it
  for about 2-3 s on a full refresh. If BUSY never rises, meter C8 (the panel's own VDD): 1.8 V long after
  power-up means the panel is powered but never heard a command, since the firmware's last command puts it
  to sleep, which turns VDD off. On the first board that was a bad joint between the MCU and DS2, fixed by
  reflowing again; check DS2 pin 8 (BS1) to ground too, since a floating BS1 selects 3-wire SPI.
- `FAST=1` counts once a second, as far as the panel keeps up: a full refresh takes longer than a second, so
  the count falls behind the clock around them.

## License

The firmware is under the [MIT License](LICENSE); the rest of the repository (the board) is under the
CERN-OHL-P v2. `tools/ppk.py` uses [ppk2-api](https://github.com/IRNAS/ppk2-api-python) (GPL-2.0), which
isn't included here; install it separately as above.
