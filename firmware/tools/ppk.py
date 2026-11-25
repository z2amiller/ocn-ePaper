#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure the board's current with a Nordic PPK2, for a few seconds or overnight.

The PPK2 samples 100,000 times a second. Each read is reduced as it arrives, so a run can last as long as you
like and memory holds only running totals and the list of wakes:
  - a progress line on the terminal every second (--every N to space them out, --quiet for none);
  - with --log PREFIX, as the run goes: PREFIX-1s.csv (a row a second) and PREFIX-events.csv (a row per wake or
    update: when, how long, peak and charge); at the end, PREFIX-report.txt;
  - at the end, a report: average current, sleep current, the wakes grouped by size with their spacing, and what
    the average costs a year.
Ctrl-C (or SIGTERM, or the USB dropping) ends the run at any time and still writes the report, so an overnight
run doesn't need timing: start it with --seconds 0 and stop it whenever. Press Ctrl-C twice to quit without one.
--summarize PREFIX rebuilds the report from the two CSVs, for a run that ended without one.

    .venv/bin/python tools/ppk.py --seconds 130                         # supply 3.0 V and measure 130 s
    .venv/bin/python tools/ppk.py --cycle 3 --seconds 130               # power off 3 s first: a clean power-up
    .venv/bin/python tools/ppk.py --ampere --volts 3.05 --seconds 0 --every 60 --log night   # until Ctrl-C
    .venv/bin/python tools/ppk.py --summarize night

Wiring on revision 0.3 (unplug the LaunchPad from J1 first: its Spy-Bi-Wire lines feed the chip):
  - Supplying the board (source meter, the default): no cell, PPK2 VOUT to TP3 (+3V0), GND to J1 pin 4.
  - In line with its own cell (--ampere): cut JP1, PPK2 VIN to JP1's cell-side pad (BT1 +), VOUT to TP3 or JP1's
    other pad, GND to J1 pin 4, cell fitted. --volts is then the cell's voltage, which the PPK2's calibration
    uses. --cycle disconnects the cell for that long. Bridge JP1 again afterwards.
  - The Mac mustn't sleep during a run, or the USB stream stops: on macOS the script holds it awake with
    caffeinate until it exits. The report's "data" line shows how much of the run was captured.

ppk2-api supplies the serial protocol and the PPK2's calibration, but decodes one sample at a time in Python,
which can't keep up with 400 kB/s: the port overflows and the 4-byte framing slips. This decodes whole reads
with numpy instead, using the same calibration formula (ppk2_api get_adc_result, without its range-switch
spike smoothing).
"""
import argparse
import csv
import datetime as dt
import os
import shutil
import signal
import subprocess
import sys
import time

import numpy as np

RATE = 100_000.0                              # PPK2 samples per second
GAP = int(0.002 * RATE)                       # samples over the threshold under 2 ms apart are one wake
HOURS_PER_YEAR = 8766.0


def calibration(ppk):
    """Per-range calibration arrays from the PPK2's own memory, indexed by measurement range 0-4."""
    m = ppk.modifiers
    return {k: np.array([float(m[k][str(r)]) for r in range(5)]) for k in ("O", "R", "UG", "GS", "GI", "S", "I")}


def decode(words, cal, vdd_v, adc_mult):
    """Raw 32-bit PPK2 words to current in uA."""
    rng = np.minimum((words >> 14) & 0x7, 4).astype(np.int64)
    adc = (words & 0x3FFF).astype(np.float64) * 4
    raw = (adc - cal["O"][rng]) * (adc_mult / cal["R"][rng])
    amps = cal["UG"][rng] * (raw * (cal["GS"][rng] * raw + cal["GI"][rng]) + (cal["S"][rng] * vdd_v + cal["I"][rng]))
    return amps * 1e6


def now_iso():
    return dt.datetime.now().isoformat(timespec="seconds")


class Stream:
    """Reduces the sample stream as it arrives: totals, one row a second, 1 ms bins, and wakes."""

    SEC_FIELDS = ["t_s", "wall", "samples", "mean_ua", "max_ua", "charge_uc", "quiet_n", "quiet_mean_ua"]
    EV_FIELDS = ["start_s", "wall", "dur_ms", "peak_ua", "charge_uc"]

    def __init__(self, threshold_ua, every=1, quiet=False, prefix=None, ms_csv=None):
        self.thr = threshold_ua
        self.every = max(1, int(every))
        self.quiet = quiet
        self.wall0 = time.time()
        self.n = 0                            # samples so far
        self.total = 0.0                      # their sum, in uA x samples
        self.quiet_n, self.quiet_sum = 0, 0.0
        self.peak = 0.0
        self.sec_parts, self.sec_n, self.sec_k = [], 0, 0
        self.win_sum, self.win_n, self.win_max = 0.0, 0, 0.0
        self.ms_rem = np.zeros(0, np.float32)
        self.open, self.ev_start, self.ev_last, self.ev_sum, self.ev_max, self.gap_sum = False, 0, 0, 0.0, 0.0, 0.0
        self.events = []                      # (start_s, dur_s, peak_ua, charge_uc)
        self.sec_w = self.ev_w = self.ms_w = None
        self.files = []
        if prefix:
            self.sec_w = self._open(f"{prefix}-1s.csv", self.SEC_FIELDS)
            self.ev_w = self._open(f"{prefix}-events.csv", self.EV_FIELDS)
        if ms_csv:
            self.ms_w = self._open(ms_csv, ["t_s", "mean_ua", "min_ua", "max_ua"])

    def _open(self, path, fields):
        f = open(path, "w", newline="")
        self.files.append(f)
        w = csv.writer(f)
        w.writerow(fields)
        f.flush()
        return (w, f)

    @staticmethod
    def _row(wf, row):
        w, f = wf
        w.writerow(row)
        f.flush()

    def feed(self, ua):
        """One read's worth of samples (float32 uA)."""
        if not len(ua):
            return
        a = ua.astype(np.float64)
        base = self.n
        self.n += len(a)
        self.total += a.sum()
        q = a < self.thr
        self.quiet_n += int(q.sum())
        self.quiet_sum += float(a[q].sum())
        self.peak = max(self.peak, float(a.max()))
        self._wakes(a, base)
        self._seconds(ua)
        if self.ms_w:
            self._ms(ua)

    # A wake is a run of samples over the threshold, with runs under GAP samples apart merged; its charge counts
    # every sample from its first to its last over-threshold sample. The run's state carries across reads.
    def _wakes(self, a, base):
        cs = np.concatenate(([0.0], np.cumsum(a)))
        idx = np.flatnonzero(a >= self.thr)
        pos = 0                               # local samples before pos are accounted for
        if len(idx):
            breaks = np.flatnonzero(np.diff(idx) > GAP)
            starts = np.concatenate(([idx[0]], idx[breaks + 1]))
            ends = np.concatenate((idx[breaks], [idx[-1]]))
            for s, e in zip(starts.tolist(), ends.tolist()):
                if self.open and base + s - self.ev_last > GAP:
                    self._close()
                if self.open:
                    self.ev_sum += self.gap_sum + (cs[s] - cs[pos])
                else:
                    self.open, self.ev_start, self.ev_sum, self.ev_max = True, base + s, 0.0, 0.0
                self.ev_sum += cs[e + 1] - cs[s]
                self.ev_max = max(self.ev_max, float(a[s:e + 1].max()))
                self.ev_last, self.gap_sum, pos = base + e, 0.0, e + 1
        if self.open:
            self.gap_sum += cs[len(a)] - cs[pos]
            if base + len(a) - 1 - self.ev_last > GAP:
                self._close()

    def _close(self):
        start = self.ev_start / RATE
        ev = (start, (self.ev_last - self.ev_start + 1) / RATE, self.ev_max, self.ev_sum / RATE)
        self.events.append(ev)
        if self.ev_w:
            wall = dt.datetime.fromtimestamp(self.wall0 + start).isoformat(timespec="seconds")
            self._row(self.ev_w, [f"{start:.3f}", wall, f"{ev[1] * 1000:.2f}", f"{ev[2]:.1f}", f"{ev[3]:.3f}"])
        self.open, self.gap_sum = False, 0.0

    def _seconds(self, ua):
        self.sec_parts.append(ua)
        self.sec_n += len(ua)
        while self.sec_n >= RATE:
            s = np.concatenate(self.sec_parts)
            head, tail = s[:int(RATE)], s[int(RATE):]
            self.sec_parts, self.sec_n = [tail], len(tail)
            self._second(head)

    def _second(self, s):
        a = s.astype(np.float64)
        q = a < self.thr
        qn = int(q.sum())
        if self.sec_w:
            qm = f"{a[q].mean():.4f}" if qn else ""
            self._row(self.sec_w, [self.sec_k, now_iso(), len(a), f"{a.mean():.4f}", f"{a.max():.1f}",
                                   f"{a.sum() / RATE:.4f}", qn, qm])
        self.sec_k += 1
        self.win_sum += a.sum()
        self.win_n += len(a)
        self.win_max = max(self.win_max, float(a.max()))
        if self.sec_k % self.every == 0 and not self.quiet:
            print(f"{self.sec_k:7d} s  avg {self.win_sum / self.win_n:10.3f} uA  peak {self.win_max:10.1f} uA"
                  f"  | run avg {self.total / self.n:9.3f} uA, {len(self.events)} wakes", flush=True)
        if self.sec_k % self.every == 0:
            self.win_sum, self.win_n, self.win_max = 0.0, 0, 0.0

    def _ms(self, ua):
        b = np.concatenate((self.ms_rem, ua))
        k = len(b) // 100
        for i, row in enumerate(b[:k * 100].reshape(k, 100)):
            t = (self.n - len(b) + i * 100) / RATE
            self._row(self.ms_w, [f"{t:.3f}", f"{row.mean():.3f}", f"{row.min():.3f}", f"{row.max():.3f}"])
        self.ms_rem = b[k * 100:]

    def finish(self):
        """Flushes the partial second and any open wake, closes the files, and returns the totals."""
        if self.open:
            self._close()
        if self.sec_n:
            self._second(np.concatenate(self.sec_parts))
            self.sec_parts, self.sec_n = [], 0
        for f in self.files:
            f.close()
        return dict(samples=self.n, total=self.total, quiet_n=self.quiet_n, quiet_sum=self.quiet_sum,
                    peak=self.peak, events=self.events)


def group(events):
    """Wakes grouped by charge: sorted, with a new group wherever the next is over 1.5 times the last."""
    if not events:
        return []
    order = sorted(events, key=lambda e: e[3])
    groups, cur = [], [order[0]]
    for e in order[1:]:
        if e[3] > 1.5 * max(cur[-1][3], 1e-9):
            groups.append(cur)
            cur = []
        cur.append(e)
    groups.append(cur)
    return groups


def fmt_charge(uc):
    return f"{uc / 1000:.3f} mC" if uc >= 1000 else f"{uc:.2f} uC"


def report(t, title, thr_ua, wall_s=None):
    """The end-of-run report, as text."""
    out = [title]
    n = t["samples"]
    if not n:
        return "\n".join(out + ["no data"])
    secs = n / RATE
    avg = t["total"] / n
    line = f"data: {secs:.1f} s ({secs / 3600:.2f} h)"
    if wall_s:
        line += f" in {wall_s:.1f} s of wall time ({100 * secs / wall_s:.1f} % captured)"
    out.append(line)
    out.append(f"average: {avg:.3f} uA   charge: {fmt_charge(t['total'] / RATE)}   peak: {t['peak']:.1f} uA")
    if t["quiet_n"]:
        out.append(f"between wakes (samples under {thr_ua:g} uA): {t['quiet_sum'] / t['quiet_n']:.3f} uA, "
                   f"{100 * t['quiet_n'] / n:.2f} % of the time")
    out.append(f"at this average: {avg * HOURS_PER_YEAR / 1000:.2f} mAh a year")
    ev = t["events"]
    out.append(f"wakes (>= {thr_ua:g} uA): {len(ev)}, {fmt_charge(sum(e[3] for e in ev))} in all")
    for g in group(ev):
        starts = sorted(e[0] for e in g)
        spacing = f", every {np.median(np.diff(starts)):.1f} s" if len(starts) > 1 else ""
        out.append(f"  {len(g):5d} x  {fmt_charge(np.mean([e[3] for e in g])):>10} each, "
                   f"{np.mean([e[1] for e in g]) * 1000:8.1f} ms, peak {np.mean([e[2] for e in g]):9.1f} uA{spacing}")
    return "\n".join(out)


def summarize(prefix, thr_ua):
    """Rebuilds the totals from PREFIX-1s.csv and PREFIX-events.csv."""
    samples, quiet_n, total, quiet_sum, peak = 0, 0, 0.0, 0.0, 0.0
    walls, events = [], []
    with open(f"{prefix}-1s.csv", newline="") as f:
        for r in csv.DictReader(f):
            k = int(r["samples"])
            samples += k
            total += float(r["mean_ua"]) * k
            peak = max(peak, float(r["max_ua"]))
            qn = int(r["quiet_n"])
            if qn:
                quiet_n += qn
                quiet_sum += float(r["quiet_mean_ua"]) * qn
            walls.append(r["wall"])
    with open(f"{prefix}-events.csv", newline="") as f:
        for r in csv.DictReader(f):
            events.append((float(r["start_s"]), float(r["dur_ms"]) / 1000, float(r["peak_ua"]), float(r["charge_uc"])))
    t = dict(samples=samples, total=total, quiet_n=quiet_n, quiet_sum=quiet_sum, peak=peak, events=events)
    wall_s = None
    if len(walls) > 1:
        wall_s = (dt.datetime.fromisoformat(walls[-1]) - dt.datetime.fromisoformat(walls[0])).total_seconds() + 1
    return report(t, f"report rebuilt from {prefix}-1s.csv and {prefix}-events.csv ({walls[0] if walls else '?'}"
                     f" to {walls[-1] if walls else '?'})", thr_ua, wall_s)


def measure(read, stream, seconds, stop):
    """Feeds stream from read() until `seconds` pass (0: forever) or stop() says so. Returns why it ended."""
    t0 = time.monotonic()
    try:
        while not stop():
            if seconds > 0 and time.monotonic() - t0 >= seconds:
                return "time up"
            stream.feed(read())
        return "interrupted"
    except OSError as e:                      # includes serial.SerialException: the PPK2 went away
        return f"stopped by a USB error ({e})"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="PPK2 serial port (default: the first PPK2 found)")
    ap.add_argument("--volts", type=float, default=3.0, help="supply voltage, or the cell's with --ampere (default 3.0)")
    ap.add_argument("--seconds", type=float, default=10.0, help="how long to measure; 0 runs until Ctrl-C (default 10)")
    ap.add_argument("--cycle", type=float, default=0.0,
                    help="first switch the supply off for this many seconds, then on (a clean power-up)")
    ap.add_argument("--spike-ua", type=float, default=50.0,
                    help="current that counts as a wake, in uA (default 50)")
    ap.add_argument("--every", type=int, default=1, help="seconds between progress lines (default 1)")
    ap.add_argument("--quiet", action="store_true", help="no progress lines")
    ap.add_argument("--off", action="store_true", help="leave the supply off at the end")
    ap.add_argument("--ampere", action="store_true",
                    help="ampere-meter mode: measure in line with the board's own cell (VIN) instead of supplying it")
    ap.add_argument("--log", metavar="PREFIX", help="write PREFIX-1s.csv and PREFIX-events.csv as the run goes, "
                                                    "and PREFIX-report.txt at the end")
    ap.add_argument("--csv", help="also write 1 ms bins (time, then mean, min and max in uA) to this CSV file")
    ap.add_argument("--raw", help="save every sample (uA, float32) to this .npy file: short runs only, it's held in memory")
    ap.add_argument("--summarize", metavar="PREFIX", help="rebuild the report from a --log run's CSVs, then exit")
    args = ap.parse_args()

    if args.summarize:
        print(summarize(args.summarize, args.spike_ua))
        return 0

    from ppk2_api.ppk2_api import PPK2_API

    port = args.port
    if not port:
        found = PPK2_API.list_devices()
        if not found:
            print("No PPK2 found.", file=sys.stderr)
            return 1
        port = found[0]

    ppk = PPK2_API(port, timeout=1, write_timeout=1, exclusive=True)
    ppk.get_modifiers()
    cal = calibration(ppk)
    if args.ampere:
        ppk.use_ampere_meter()                 # before anything else, so VOUT is never driven against the cell
        ppk.set_source_voltage(int(round(args.volts * 1000)))
    else:
        ppk.set_source_voltage(int(round(args.volts * 1000)))
        ppk.use_source_meter()

    if args.cycle > 0:
        ppk.toggle_DUT_power("OFF")
        print(f"supply off for {args.cycle:g} s")
        time.sleep(args.cycle)
    ppk.toggle_DUT_power("ON")
    mode = (f"cell connected through the PPK2 (calibrated for {args.volts:g} V)" if args.ampere
            else f"supply on at {args.volts:g} V")
    print(f"{mode}, port {port}, started {now_iso()}"
          + (", until Ctrl-C" if args.seconds <= 0 else f", for {args.seconds:g} s"), flush=True)

    # Ctrl-C or SIGTERM: finish the current read, then report. A second Ctrl-C quits at once.
    stops = []

    def on_signal(signum, _frame):
        if stops:
            raise KeyboardInterrupt
        stops.append(signum)
        print("\nstopping: finishing the report (Ctrl-C again to quit without it)", file=sys.stderr, flush=True)

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    stream = Stream(args.spike_ua, args.every, args.quiet, args.log, args.csv)
    chunks = [] if args.raw else None
    leftover = b""

    def read():
        nonlocal leftover
        data = ppk.ser.read(ppk.ser.in_waiting or 1)
        if not data:
            return np.zeros(0, np.float32)
        data = leftover + data
        cut = len(data) - len(data) % 4
        leftover = data[cut:]
        ua = decode(np.frombuffer(data[:cut], dtype="<u4"), cal, args.volts, ppk.adc_mult).astype(np.float32)
        if chunks is not None:
            chunks.append(ua)
        return ua

    # Keep the Mac awake for the run (macOS: caffeinate exits with this process).
    awake = None
    if sys.platform == "darwin" and shutil.which("caffeinate"):
        awake = subprocess.Popen(["caffeinate", "-i", "-w", str(os.getpid())],
                                 stdin=subprocess.DEVNULL, start_new_session=True)

    ppk.start_measuring()
    t_wall = time.monotonic()
    try:
        why = measure(read, stream, args.seconds, lambda: bool(stops))
    except KeyboardInterrupt:
        why = None
    wall_s = time.monotonic() - t_wall
    try:
        ppk.stop_measuring()
        if args.off:
            ppk.toggle_DUT_power("OFF")
    except OSError:
        pass
    if awake:
        awake.terminate()
    if why is None:
        print("\nquit without a report", file=sys.stderr)
        return 130

    totals = stream.finish()
    text = report(totals, f"run {why}, ended {now_iso()}", args.spike_ua, wall_s)
    print()
    print(text)
    if args.log:
        with open(f"{args.log}-report.txt", "w") as f:
            f.write(text + "\n")
        print(f"\nlogged to {args.log}-1s.csv, {args.log}-events.csv and {args.log}-report.txt")
    if args.raw:
        allv = np.concatenate(chunks) if chunks else np.zeros(0, np.float32)
        np.save(args.raw, allv)
        print(f"wrote {len(allv)} samples to {args.raw}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
