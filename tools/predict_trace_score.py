#!/usr/bin/env python3
# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: Apache-2.0
"""Score the per-weave eye-prediction traces written by the Leia SR plug-in.

The plug-in's recorder (``DXR_LEIA_SR_PREDICT_TRACE=1``,
``src/drv_leia/leia_sr_predict_trace.cpp``) writes one CSV per weaver per run.
This reads one or more of them and answers the only question the trace exists
for: **how far off was the predicted eye position, at the instant the frame was
actually shown?**

It answers it OFFLINE, against the raw tracker stream, because that is the one
ground truth in the file that no predictor produced.

Method
------
For every ``W`` row:

* the evaluation instant is ``scanout_us`` -- ``now + horizon``, computed the
  same way on both arms so the two are scored against the same question;
* the two ``T`` rows that bracket it give the measured eye positions, linearly
  interpolated to that instant;
* a bracket wider than twice the tracker period (estimated from the ``T``
  stream itself) is DROPPED rather than interpolated across, and so is any
  instant inside a ``USER_LOST .. USER_FOUND`` span -- there the SDK animates
  the pair toward the display default, and scoring that would measure the
  fallback animation instead of a prediction.

The PRIMARY metric is on the eye MIDPOINT ``(L+R)/2``, which is invariant to
the SDK getter's left/right swap. Per-eye numbers are secondary and are
reported separately for ``swap_flag == 0`` and ``swap_flag == 1``, never pooled.

Stdlib only -- csv, math, statistics. No numpy.

Usage
-----
    python3 tools/predict_trace_score.py predict_trace_legacy_*.csv \
                                         predict_trace_target_*.csv
    python3 tools/predict_trace_score.py --bins 0,50,150,400 trace.csv
"""

import argparse
import csv
import glob
import math
import os
import statistics
import sys
from collections import OrderedDict

AXES = ("x", "y", "z")

SR_EVENT_USER_FOUND = 16
SR_EVENT_USER_LOST = 17

CAVEATS = [
    "The REFERENCE is not ground truth. It is each weave's own srEyeTrackerPredict(0) on a "
    "separate tracker handle -- the filter's smoothed estimate for now + min(1/120 s, "
    "max_prediction_scene_s) (-4.36 ms on this profile), computed the SAME way in both arms. "
    "No raw eye measurement exists in the C99 surface (enablePrediction is ignored by SDK "
    "1584). ABSOLUTE errors are therefore comparison-only; the ARM-TO-ARM comparison is "
    "sound because both arms are scored against the same reference.",
    "arm A (legacy) bias is ARITHMETIC, not a defect: the relative path predicts for "
    "now-4.36 ms (max_prediction_scene_s) while the frame is shown at now+horizon, so a "
    "constant offset is expected. Judge the arms on SPREAD and RMS, not on mean bias.",
    "Two consecutive runs are TWO SAMPLES, not a paired comparison: nothing here pairs a "
    "legacy weave with a target weave, and the viewer moved differently in each run.",
    "Instrumented runs add THREE predictor calls per weave (the getter, the reference "
    "predict, plus clock reads), one of which emits a T row. They are comparable to EACH "
    "OTHER and NOT to an uninstrumented baseline.",
    "T rows are DIAGNOSTIC ONLY: the tracker callback stream is an echo of predict() calls, "
    "not measurements. Nothing here scores against them.",
]

DEFAULT_SPEED_BINS = (0.0, 25.0, 75.0, 200.0, float("inf"))


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------


class Trace(object):
    """One CSV: its header keys, its three streams, and its trailer."""

    def __init__(self, path):
        self.path = path
        self.meta = OrderedDict()
        self.trailer = ""
        self.t_untimed = 0  # T rows with timeUs == 0: untracked default pairs, no capture time
        self.s_mapped = 0  # S rows whose timestamp was mapped through the header clock pair
        self.t_rows = []  # (time_us, lx,ly,lz, rx,ry,rz)
        self.s_rows = []  # (time_us, event_type)
        self.w_rows = []  # dict
        self.malformed = 0

    @property
    def arm(self):
        return self.meta.get("arm", "unknown")


def _f(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _i(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


FILETIME_UNIX_EPOCH_100NS = 116444736000000000  # 1970-01-01 in FILETIME ticks


def _map_s_time(tr, v):
    """S rows were observed (2026-09-16 smoke) NOT to be in the srGetTimeUs
    domain: they looked like 100 ns ticks since the Unix epoch. Map such a
    value onto the SDK clock through the header clock pair; leave anything
    already plausible (< 1e15) alone."""
    if v < 10 ** 15:
        return v
    try:
        sr_us = int(tr.meta["clock_pair_sr_us"])
        ft = int(tr.meta["clock_pair_filetime_100ns_since_1601"])
    except (KeyError, ValueError):
        return v
    unix100_at_pair = ft - FILETIME_UNIX_EPOCH_100NS
    tr.s_mapped += 1
    return sr_us + (v - unix100_at_pair) // 10


def read_trace(path):
    tr = Trace(path)
    with open(path, "r", newline="", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if not line:
                continue
            if line.startswith("#"):
                body = line[1:].strip()
                if body.startswith("trailer"):
                    tr.trailer = body
                elif "=" in body:
                    key, _, val = body.partition("=")
                    # Header keys are read BY NAME, never by line number: the
                    # recorder defers the header until it has something to say
                    # about the clock gate, so position is not meaningful.
                    tr.meta.setdefault(key.strip(), val.strip())
                continue

            row = next(csv.reader([line]))
            if not row:
                continue
            kind = row[0]
            try:
                if kind == "T" and len(row) >= 9:
                    if int(row[2]) == 0:
                        # No capture time: the SDK delivers default pairs
                        # when nobody is tracked. Not ground truth.
                        tr.t_untimed += 1
                        continue
                    tr.t_rows.append(
                        (
                            int(row[2]),
                            float(row[3]),
                            float(row[4]),
                            float(row[5]),
                            float(row[6]),
                            float(row[7]),
                            float(row[8]),
                        )
                    )
                elif kind == "S" and len(row) >= 3:
                    tr.s_rows.append((_map_s_time(tr, int(row[2])), int(row[1])))
                elif kind == "W" and len(row) >= 19:
                    tr.w_rows.append(
                        {
                            "seq": _i(row[1]),
                            "arm": row[2],
                            "now_us": _i(row[3]),
                            "push_us": _i(row[4]),
                            "scanout_us": _i(row[5]),
                            "pushed": _i(row[6]),
                            "last_set_latency_us": _i(row[7]),
                            "target_us": _i(row[8]),
                            "horizon_us": _i(row[9]),
                            "resolved_us": _i(row[10]),
                            "read_now_us": _i(row[11]),
                            "pl": (_f(row[12]), _f(row[13]), _f(row[14])),
                            "pr": (_f(row[15]), _f(row[16]), _f(row[17])),
                            "swap": _i(row[18]),
                            # format 2 reference fields; absent on format-1 files
                            "ref_now_us": _i(row[19]) if len(row) >= 27 else None,
                            "rl": (_f(row[20]), _f(row[21]), _f(row[22])) if len(row) >= 27 else None,
                            "rr": (_f(row[23]), _f(row[24]), _f(row[25])) if len(row) >= 27 else None,
                            "ref_ok": _i(row[26]) if len(row) >= 27 else 0,
                        }
                    )
                else:
                    tr.malformed += 1
            except (ValueError, IndexError):
                tr.malformed += 1

    tr.t_rows.sort(key=lambda r: r[0])
    tr.s_rows.sort(key=lambda r: r[0])
    return tr


# ---------------------------------------------------------------------------
# Ground truth
# ---------------------------------------------------------------------------


def tracker_period_us(t_rows):
    """Median inter-sample gap. The SDK exposes no nominal rate, so the stream
    has to describe itself -- median rather than mean so one stall does not
    inflate the tolerance that is supposed to catch stalls."""
    if len(t_rows) < 3:
        return None
    gaps = [b[0] - a[0] for a, b in zip(t_rows, t_rows[1:]) if b[0] > a[0]]
    if not gaps:
        return None
    return float(statistics.median(gaps))


def lost_spans(s_rows):
    """[(start_us, end_us)] spans with no tracked user.

    A LOST with no following FOUND runs to infinity -- the user never came
    back, and every weave after it is untracked.
    """
    # USER_FOUND / USER_LOST are PERSISTENT events in the SDK: the current
    # state is replayed to every new listener with its ORIGINAL timestamp, so
    # the first event is state rather than a transition, may predate the run,
    # and FOUND/LOST need not alternate. Tolerate repeats and a leading event
    # of either type; only a LOST->FOUND transition closes a span.
    spans = []
    open_at = None
    state = None
    for time_us, ev in s_rows:
        if ev == state:
            continue  # repeat of the current state: not a transition
        if ev == SR_EVENT_USER_LOST:
            open_at = time_us
        elif ev == SR_EVENT_USER_FOUND and open_at is not None:
            spans.append((open_at, time_us))
            open_at = None
        state = ev
    if open_at is not None:
        spans.append((open_at, float("inf")))
    return spans


def in_any_span(t, spans):
    for a, b in spans:
        if a <= t <= b:
            return True
    return False


def bracket(t_rows, t_us, lo_hint=0):
    """Index i with t_rows[i].time <= t_us < t_rows[i+1].time, or None.

    Linear scan from a hint: the W rows arrive in time order, so this is O(n)
    over the whole file rather than O(n log n) per row, and it keeps the code
    obvious.
    """
    i = lo_hint
    n = len(t_rows)
    while i + 1 < n and t_rows[i + 1][0] <= t_us:
        i += 1
    if i + 1 >= n:
        return None
    if t_rows[i][0] > t_us:
        return None
    return i


def interp(t_rows, i, t_us):
    a = t_rows[i]
    b = t_rows[i + 1]
    span = float(b[0] - a[0])
    if span <= 0.0:
        return None, 0.0
    f = (t_us - a[0]) / span
    vals = tuple(a[1 + k] + (b[1 + k] - a[1 + k]) * f for k in range(6))
    return vals, span


def speed_mm_s(t_rows, i):
    """Midpoint speed across the bracketing pair, mm/s. Head velocity is what
    a predictor has to extrapolate, so it is the axis the RMS is binned on --
    leash saturation only shows up against it."""
    a = t_rows[i]
    b = t_rows[i + 1]
    dt = (b[0] - a[0]) / 1e6
    if dt <= 0.0:
        return 0.0
    d = 0.0
    for k in range(3):
        am = (a[1 + k] + a[4 + k]) * 0.5
        bm = (b[1 + k] + b[4 + k]) * 0.5
        d += (bm - am) ** 2
    return math.sqrt(d) / dt


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------


def stats(vals):
    if not vals:
        return None
    n = len(vals)
    mean = sum(vals) / n
    rms = math.sqrt(sum(v * v for v in vals) / n)
    sd = statistics.pstdev(vals) if n > 1 else 0.0
    return {"n": n, "mean": mean, "rms": rms, "std": sd}


def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
    sxx = sum((a - mx) ** 2 for a in xs)
    syy = sum((b - my) ** 2 for b in ys)
    if sxx <= 0.0 or syy <= 0.0:
        return None
    return sxy / math.sqrt(sxx * syy)


def fmt_stats(label, st):
    if st is None:
        return "    %-22s (no samples)" % label
    return "    %-22s n=%-6d rms=%8.3f  bias=%+8.3f  sd=%7.3f" % (
        label,
        st["n"],
        st["rms"],
        st["mean"],
        st["std"],
    )


# ---------------------------------------------------------------------------
# Scoring
# ---------------------------------------------------------------------------


def reference_rows(tr):
    """The reference series: every weave's own srEyeTrackerPredict(0) sample,
    anchored at ref_now_us + min(8333, max_prediction_scene_s * 1e6). predict(0)
    resolves to min(1/120 s, maxPredictionScene_s): on this profile the clamp
    (-4.36 ms) wins; on a profile without it the 1/120 s term would, and
    anchoring on the clamp alone would mis-place the whole series. Same tuple
    shape as the T rows so bracket/interp/speed work unchanged."""
    try:
        scene_s = float(tr.meta.get("max_prediction_scene_s", "-0.00436"))
    except ValueError:
        scene_s = -0.00436
    off = min(8333.0, scene_s * 1e6)
    rows = []
    for w in tr.w_rows:
        if not w.get("ref_ok") or not w.get("ref_now_us"):
            continue
        rows.append((int(w["ref_now_us"] + off),) + tuple(w["rl"]) + tuple(w["rr"]))
    rows.sort(key=lambda r: r[0])
    return rows, off


def score_trace(tr, bins):
    ref_rows, ref_off = reference_rows(tr)
    # The period of the REFERENCE series (one sample per weave, so ~the weave
    # period); scanout is 30-60 ms ahead and is bracketed by later weaves.
    period = tracker_period_us(ref_rows)
    tol = 2.0 * period if period else None
    spans = lost_spans(tr.s_rows)

    res = {
        "trace": tr,
        "period_us": period,
        "ref_rows": len(ref_rows),
        "ref_offset_us": ref_off,
        "lost_spans": len(spans),
        "kept": 0,
        "drop_no_bracket": 0,
        "drop_gap": 0,
        "drop_untracked": 0,
        "drop_no_horizon": 0,
        "drop_bad_row": 0,
        # midpoint error per axis, plus magnitude
        "mid": {a: [] for a in AXES},
        "mid_mag": [],
        # per-eye, kept apart by swap_flag -- never pooled
        "eye": {0: {"L": {a: [] for a in AXES}, "R": {a: [] for a in AXES}},
                1: {"L": {a: [] for a in AXES}, "R": {a: [] for a in AXES}}},
        "eye_n": {0: 0, 1: 0},
        "bins": [{"lo": bins[k], "hi": bins[k + 1], "vals": []} for k in range(len(bins) - 1)],
        # Horizon bins (push_us, ms). The effect under test is a function of the
        # horizon -- the expected advance on this profile is horizon + 4.36 ms,
        # a load-dependent range of roughly 21-64 ms (16.5 ms idle horizon up to
        # the 60 ms ceiling under load) -- so pooling across a 4x horizon range
        # would blur exactly the thing being measured.
        "hbins": [{"lo": lo, "hi": hi, "vals": []} for lo, hi in
                  ((0.0, 20.0), (20.0, 40.0), (40.0, 60.0), (60.0, float("inf")))],
        "corr_slack": [],
        "corr_absmag": [],
    }

    hint = 0
    for w in tr.w_rows:
        t_us = w["scanout_us"]
        if t_us is None or w["pl"][0] is None or w["pr"][0] is None:
            res["drop_bad_row"] += 1
            continue
        if not w["push_us"]:
            # No horizon was computed this weave, so `scanout` is just `now`
            # and there is no photon instant to score against. Not an error --
            # it is the pre-engagement first weave (seq=1: push_us=0, and on
            # the target arm resolved_us = the weaver's default one frame).
            # Filter on push_us, NOT on `pushed`: on the legacy arm pushed=0 is
            # routine (the deadband suppressed the setLatency call while
            # push_us is a real horizon) and those rows are valid.
            res["drop_no_horizon"] += 1
            continue
        if in_any_span(t_us, spans):
            res["drop_untracked"] += 1
            continue

        i = bracket(ref_rows, t_us, hint)
        if i is None:
            res["drop_no_bracket"] += 1
            continue
        hint = i
        vals, span = interp(ref_rows, i, t_us)
        if vals is None:
            res["drop_no_bracket"] += 1
            continue
        if tol is not None and span > tol:
            # A gap wider than two tracker periods is a stall, and a straight
            # line across a stall is a fabricated measurement.
            res["drop_gap"] += 1
            continue

        res["kept"] += 1
        meas_l = vals[0:3]
        meas_r = vals[3:6]

        # PRIMARY: the midpoint. Invariant to the getter's L/R swap, so it is
        # the one number both swap populations can be pooled into honestly.
        mag2 = 0.0
        for k, ax in enumerate(AXES):
            pm = (w["pl"][k] + w["pr"][k]) * 0.5
            mm = (meas_l[k] + meas_r[k]) * 0.5
            e = pm - mm
            res["mid"][ax].append(e)
            mag2 += e * e
        mag = math.sqrt(mag2)
        res["mid_mag"].append(mag)

        # SECONDARY: per eye, bucketed by swap_flag and never mixed.
        sw = 1 if w["swap"] else 0
        res["eye_n"][sw] += 1
        for k, ax in enumerate(AXES):
            res["eye"][sw]["L"][ax].append(w["pl"][k] - meas_l[k])
            res["eye"][sw]["R"][ax].append(w["pr"][k] - meas_r[k])

        # Head-speed bins, so leash saturation is visible rather than averaged
        # into the aggregate.
        v = speed_mm_s(ref_rows, i)
        for b in res["bins"]:
            if b["lo"] <= v < b["hi"]:
                b["vals"].append(mag)
                break
        h_ms = w["push_us"] / 1000.0
        for b in res["hbins"]:
            if b["lo"] <= h_ms < b["hi"]:
                b["vals"].append(mag)
                break

        # Target arm only: does the error track how far the weaver's resolved
        # horizon drifted from the one we asked for?
        if tr.arm == "target" and w["resolved_us"]:
            res["corr_slack"].append(float(w["resolved_us"] - w["push_us"]))
            res["corr_absmag"].append(mag)

    return res


def print_trace_report(res):
    tr = res["trace"]
    print("")
    print("=" * 78)
    print("arm=%s  file=%s" % (tr.arm, os.path.basename(tr.path)))
    for k in (
        "weaver_arm",
        "plugin_version",
        "sr_sdk_pin",
        "sr_runtime_version",
        "clock_gate_delta_us",
        "panel_id",
        "monitor_device",
        "monitor_orientation_deg",
        "tracker_nominal_hz",
    ):
        if k in tr.meta:
            print("  %-24s %s" % (k, tr.meta[k]))
    if tr.meta.get("tracker_nominal_hz") == "0":
        print("  %-24s %s" % ("", "(0 = SDK exposes no tracker rate; estimated from the T stream)"))
    if res["period_us"]:
        print(
            "  %-24s %.0f us (%.1f Hz), bracket tolerance %.0f us"
            % (
                "reference period (est)",
                res["period_us"],
                1e6 / res["period_us"],
                2.0 * res["period_us"],
            )
        )
    print("  %-24s %d samples, anchored at ref_now_us %+.0f us (T rows: %d, diagnostic only)"
          % ("reference series", res["ref_rows"], res["ref_offset_us"], len(tr.t_rows)))
    if tr.trailer:
        print("  %-24s %s" % ("recorder trailer", tr.trailer))
    else:
        print("  %-24s %s" % ("recorder trailer", "MISSING - player hard-killed? drop counts unknown"))
    if tr.t_untimed:
        print("  %-24s %d (timeUs == 0: untracked default pairs, excluded)" % ("T rows untimed", tr.t_untimed))
    if tr.s_mapped:
        print("  %-24s %d (mapped through header clock pair; S domain advisory)" % ("S rows mapped", tr.s_mapped))
    if tr.malformed:
        print("  %-24s %d" % ("malformed lines", tr.malformed))

    print(
        "  rows: T=%d S=%d W=%d | kept=%d  dropped: no-bracket=%d gap>2T=%d untracked=%d "
        "no-horizon=%d bad=%d  (untracked spans: %d)"
        % (
            len(tr.t_rows),
            len(tr.s_rows),
            len(tr.w_rows),
            res["kept"],
            res["drop_no_bracket"],
            res["drop_gap"],
            res["drop_untracked"],
            res["drop_no_horizon"],
            res["drop_bad_row"],
            res["lost_spans"],
        )
    )
    if res["kept"] == 0:
        print("  NOTHING SCORED -- do not read a verdict out of this file.")
        return

    print("")
    print("  PRIMARY -- midpoint (L+R)/2 error, mm (predicted - measured at scanout):")
    for ax in AXES:
        print(fmt_stats("mid " + ax, stats(res["mid"][ax])))
    print(fmt_stats("mid |error|", stats(res["mid_mag"])))

    print("")
    print("  SECONDARY -- per eye, reported separately by swap_flag (never pooled):")
    for sw in (0, 1):
        n = res["eye_n"][sw]
        if n == 0:
            print("    swap_flag=%d: no samples" % sw)
            continue
        print("    swap_flag=%d  (n=%d)" % (sw, n))
        for side in ("L", "R"):
            for ax in AXES:
                print(fmt_stats("  %s %s" % (side, ax), stats(res["eye"][sw][side][ax])))

    print("")
    print("  Head-speed bins (midpoint speed across the bracketing pair, mm/s):")
    for b in res["bins"]:
        hi = "inf" if b["hi"] == float("inf") else "%.0f" % b["hi"]
        st = stats(b["vals"])
        label = "%.0f-%s" % (b["lo"], hi)
        if st is None:
            print("    %-22s (no samples)" % label)
        else:
            print(
                "    %-22s n=%-6d rms|error|=%8.3f  mean=%8.3f"
                % (label, st["n"], st["rms"], st["mean"])
            )

    print("  Horizon bins (push_us, ms; expected advance on this profile = horizon + 4.36 ms, "
          "a load-dependent 21-64 ms range):")
    for b in res["hbins"]:
        hi = "inf" if b["hi"] == float("inf") else "%.0f" % b["hi"]
        st = stats(b["vals"])
        label = "%.0f-%s ms" % (b["lo"], hi)
        if st is None:
            print("    %-22s (no samples)" % label)
        else:
            print(
                "    %-22s n=%-6d rms|error|=%8.3f  mean=%8.3f"
                % (label, st["n"], st["rms"], st["mean"])
            )

    if tr.arm == "target":
        r = pearson(res["corr_slack"], res["corr_absmag"])
        print("")
        if r is None:
            print("  |error| vs (resolved_us - push_us): not enough variation to correlate")
        else:
            print(
                "  |error| vs (resolved_us - push_us): pearson r = %+.3f over n=%d"
                % (r, len(res["corr_slack"]))
            )
            print(
                "      (resolved is the horizon the weave RESOLVED to; a strong positive r "
                "means the error grows with how far the weaver's own resolution drifted "
                "from the horizon we asked for)"
            )


def print_arm_comparison(results):
    by_arm = OrderedDict()
    for res in results:
        by_arm.setdefault(res["trace"].arm, []).append(res)
    if len(by_arm) < 2:
        return
    print("")
    print("=" * 78)
    print("ARM SUMMARY -- primary metric only (midpoint |error|, mm)")
    for arm, group in by_arm.items():
        vals = []
        for res in group:
            vals.extend(res["mid_mag"])
        st = stats(vals)
        if st is None:
            print("  %-8s (nothing scored)" % arm)
        else:
            print(
                "  %-8s files=%-2d n=%-6d rms=%8.3f  mean=%8.3f  sd=%7.3f"
                % (arm, len(group), st["n"], st["rms"], st["mean"], st["std"])
            )
    print("")
    print("  Compare SPREAD and RMS. See the caveats printed above; in particular the")
    print("  legacy arm's mean is expected to be offset and is not by itself a verdict.")


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Score Leia SR per-weave eye-prediction traces (offline, stdlib only)."
    )
    ap.add_argument("paths", nargs="+", help="one or more predict_trace_*.csv (globs allowed)")
    ap.add_argument(
        "--bins",
        default=",".join("%g" % b for b in DEFAULT_SPEED_BINS[:-1]),
        help="comma-separated head-speed bin edges in mm/s; an open top bin is appended",
    )
    args = ap.parse_args(argv)

    files = []
    for p in args.paths:
        hits = sorted(glob.glob(p))
        files.extend(hits if hits else [p])

    edges = [float(x) for x in args.bins.split(",") if x.strip() != ""]
    edges.sort()
    if not edges or edges[0] > 0.0:
        edges.insert(0, 0.0)
    edges.append(float("inf"))

    print("READ THIS BEFORE THE NUMBERS")
    for c in CAVEATS:
        print("  * " + c)

    results = []
    for path in files:
        if not os.path.isfile(path):
            print("\n!! missing: %s" % path, file=sys.stderr)
            continue
        tr = read_trace(path)
        res = score_trace(tr, edges)
        results.append(res)
        print_trace_report(res)

    if not results:
        return 1
    print_arm_comparison(results)
    print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
