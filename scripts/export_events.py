#!/usr/bin/env python3
"""Export and summarise zoigraph's phase-2 events table.

The events table is populated by Database::log_event (see
src/persistence/db.cpp). Each row is one of:
    phantom_spawn / phantom_decay / phantom_pin / node_edit / bones_throw
plus arbitrary future kinds. Payload is JSON whose shape varies by kind.

Two modes, on by default:
    --out PATH      write the rows out as CSV
    --summary       print derived metrics (pin rate, time-to-pin
                    distribution, pin-then-edit-within-60s rate)

Both run by default. Pass --no-summary to skip the printed stats.

Usage:
    python3 scripts/export_events.py
    python3 scripts/export_events.py --db projects/default.db --out events.csv
    python3 scripts/export_events.py --db projects/default.db --no-summary --out events.csv

See llm_bridge.md for the full phase-2 plan and stop criteria.
"""

import argparse
import csv
import json
import math
import sqlite3
import statistics
import sys
from collections import Counter
from pathlib import Path


def wilson_interval(successes: int, n: int,
                    confidence: float = 0.95) -> tuple[float, float]:
    """Wilson score interval for a binomial proportion, as (lo, hi) in [0, 1].

    The small-sample-honest replacement for the naive p ± z·sqrt(p(1-p)/n):
    it never escapes [0, 1] and stays sensible at p near 0/1 and at tiny n --
    exactly the regime phase-2 pin rates live in, where a raw percentage from
    a handful of phantoms lies. With n == 0 the proportion is undefined, so the
    interval is the whole [0, 1] (maximal ignorance).

    >>> lo, hi = wilson_interval(2, 8)          # 25% from only 8 trials
    >>> round(lo, 4), round(hi, 4)
    (0.0715, 0.5907)
    >>> wilson_interval(0, 10)[0]               # no successes -> lo pinned at 0
    0.0
    >>> wilson_interval(10, 10)[1]              # all successes -> hi pinned at 1
    1.0
    >>> wilson_interval(0, 0)                   # undefined -> full range
    (0.0, 1.0)
    """
    if n <= 0:
        return (0.0, 1.0)
    z = statistics.NormalDist().inv_cdf(1 - (1 - confidence) / 2)
    p = successes / n
    z2 = z * z
    denom = 1 + z2 / n
    center = (p + z2 / (2 * n)) / denom
    half = (z / denom) * math.sqrt(p * (1 - p) / n + z2 / (4 * n * n))
    # k == 0 and k == n have exact bounds (0 and 1); pin them so float noise
    # doesn't leak a 2.8e-17 "lower bound".
    lo = 0.0 if successes <= 0 else max(0.0, center - half)
    hi = 1.0 if successes >= n else min(1.0, center + half)
    return (lo, hi)


def two_proportion_z_test(k1: int, n1: int,
                          k2: int, n2: int) -> tuple[float, float] | None:
    """Pooled two-proportion z-test of H0: p1 == p2. Returns (z, p_two_sided).

    Uses the POOLED proportion for the standard error -- the correct form when
    the null is equality (the common mistake is plugging the two separate
    sample proportions into the SE). `p_two_sided` is the chance of a gap at
    least this large under H0; small (< 0.05 by convention) means the two rates
    differ by more than sampling noise. Returns None when the test is undefined:
    an empty arm, or every pooled trial a success/failure (zero variance).

    >>> z, p = two_proportion_z_test(6, 20, 4, 20)   # 30% vs 20%, n=20 each
    >>> round(z, 4), round(p, 4)
    (0.7303, 0.4652)
    >>> two_proportion_z_test(0, 0, 1, 5) is None     # empty arm
    True
    >>> two_proportion_z_test(0, 10, 0, 10) is None   # zero pooled variance
    True
    """
    if n1 <= 0 or n2 <= 0:
        return None
    p_pool = (k1 + k2) / (n1 + n2)
    var = p_pool * (1 - p_pool) * (1 / n1 + 1 / n2)
    if var <= 0:
        return None
    z = (k1 / n1 - k2 / n2) / math.sqrt(var)
    p_two = 2 * statistics.NormalDist().cdf(-abs(z))  # robust upper-tail form
    return (z, p_two)


def load_events(db_path: str) -> list[dict]:
    con = sqlite3.connect(db_path)
    rows = con.execute(
        "SELECT ts, kind, node_id, payload FROM events ORDER BY ts;"
    ).fetchall()
    con.close()
    out = []
    for ts, kind, node_id, payload in rows:
        try:
            parsed = json.loads(payload) if payload else {}
        except json.JSONDecodeError:
            parsed = {}
        out.append({
            "ts":      ts,
            "kind":    kind,
            "node_id": node_id,
            "payload": payload or "",
            "parsed":  parsed,
        })
    return out


def write_csv(events: list[dict], out_path: str) -> None:
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts", "kind", "node_id", "payload"])
        for e in events:
            w.writerow([
                f"{e['ts']:.3f}",
                e["kind"],
                "" if e["node_id"] is None else e["node_id"],
                e["payload"],
            ])


def summarize(events: list[dict]) -> None:
    n = len(events)
    by_kind = Counter(e["kind"] for e in events)

    spawned: dict = {}   # phantom_id -> spawn ts
    pinned:  dict = {}   # phantom_id -> pin ts
    decayed: dict = {}   # phantom_id -> decay ts
    source_of: dict = {} # phantom_id -> source tag (from spawn payload)
    dup_spawns = 0       # same phantom_id spawned twice -> dict overwrite
    orphans = 0          # pin/decay with no matching spawn (cross-project
                         # pollution or pre-instrumentation rows)
    for e in events:
        p = e["parsed"]
        if e["kind"] == "phantom_spawn":
            pid = p.get("phantom_id")
            if pid in spawned:
                dup_spawns += 1
            spawned[pid] = e["ts"]
            source_of[pid] = p.get("source") or "(unsourced)"
        elif e["kind"] == "phantom_pin":
            pid = p.get("phantom_id")
            if pid not in spawned:
                orphans += 1
            pinned[pid] = e["ts"]
        elif e["kind"] == "phantom_decay":
            pid = p.get("phantom_id")
            if pid not in spawned:
                orphans += 1
            decayed[pid] = e["ts"]

    n_spawned = len(spawned)
    if dup_spawns:
        print(f"  WARNING: {dup_spawns} duplicate phantom_id spawn(s) -- "
              f"id collisions overwrite lifecycle tracking; every rate below "
              f"is unreliable. Fix the emitter's id minting.", file=sys.stderr)
    if orphans:
        print(f"  WARNING: {orphans} pin/decay event(s) reference a "
              f"phantom_id never spawned in this DB -- possible "
              f"cross-project contamination.", file=sys.stderr)
    pin_rate   = (100 * len(pinned)  / n_spawned) if n_spawned else 0.0
    decay_rate = (100 * len(decayed) / n_spawned) if n_spawned else 0.0
    pin_lo, pin_hi = wilson_interval(len(pinned), n_spawned)  # proportions

    ttps = [
        e["parsed"].get("time_to_pin_s") for e in events
        if e["kind"] == "phantom_pin"
        and isinstance(e["parsed"].get("time_to_pin_s"), (int, float))
    ]

    # Pin-then-edit-within-60s: for each pin, was there a node_edit on the
    # promoted new_node_id within 60 seconds of the pin? Pins counted once
    # regardless of edit count.
    pin_ts_by_node: dict = {}
    for e in events:
        if e["kind"] == "phantom_pin":
            nn = e["parsed"].get("new_node_id")
            if nn is not None:
                pin_ts_by_node[nn] = e["ts"]
    pinned_then_edited: set = set()
    for e in events:
        if e["kind"] != "node_edit":
            continue
        nid = e["node_id"]
        if nid in pin_ts_by_node:
            delta = e["ts"] - pin_ts_by_node[nid]
            if 0 < delta <= 60:
                pinned_then_edited.add(nid)
    pte_rate = (100 * len(pinned_then_edited) / len(pin_ts_by_node)
                if pin_ts_by_node else 0.0)

    print("=== summary ===")
    print(f"  total events:        {n}")
    print(f"  by kind:")
    for k, c in by_kind.most_common():
        print(f"    {k:20s} {c:>5}")
    print()
    print(f"  phantoms spawned:    {n_spawned}")
    print(f"  phantoms pinned:     {len(pinned):>3}   ({pin_rate:5.1f}%)"
          f"   95% CI [{100*pin_lo:.1f}, {100*pin_hi:.1f}]%")
    print(f"  phantoms decayed:    {len(decayed):>3}   ({decay_rate:5.1f}%)")
    in_flight = n_spawned - len(pinned) - len(decayed)
    if in_flight > 0:
        print(f"  in flight / unknown: {in_flight}")
    print()
    if ttps:
        print(f"  time-to-pin (s):")
        print(f"    median: {statistics.median(ttps):6.2f}")
        print(f"    mean:   {statistics.mean(ttps):6.2f}")
        print(f"    min:    {min(ttps):6.2f}")
        print(f"    max:    {max(ttps):6.2f}")
        print()
    print(f"  pin-then-edit-within-60s: "
          f"{len(pinned_then_edited)}/{len(pin_ts_by_node)} ({pte_rate:.1f}%)")
    print(f"  bones throws:        {by_kind.get('bones_throw', 0)}")

    # By-source breakdown. Required for the ceiling-vs-floor (Claude vs
    # Ollama) comparison the plan calls out as Phase 2's most informative
    # metric. Sources are tags self-reported by the emitter, so any
    # post-hoc disagreement means the analyst, not the model, picks who's
    # at fault. Sorted by spawn count so dominant emitters surface first.
    if source_of:
        per_source: dict[str, dict[str, int]] = {}
        for pid, src in source_of.items():
            stats = per_source.setdefault(src, {"spawned": 0, "pinned": 0, "decayed": 0})
            stats["spawned"] += 1
            if pid in pinned:  stats["pinned"]  += 1
            if pid in decayed: stats["decayed"] += 1
        print()
        print(f"  by source (pin rate = pinned / spawned):")
        print(f"    {'source':<28s} {'spawned':>8s} {'pinned':>8s} "
              f"{'decayed':>8s} {'pin %':>7s} {'95% CI %':>16s}")
        for src in sorted(per_source.keys(),
                          key=lambda s: -per_source[s]["spawned"]):
            st = per_source[src]
            pr = (100 * st["pinned"] / st["spawned"]) if st["spawned"] else 0
            lo, hi = wilson_interval(st["pinned"], st["spawned"])
            print(f"    {src:<28s} {st['spawned']:>8d} {st['pinned']:>8d} "
                  f"{st['decayed']:>8d} {pr:>6.1f}% "
                  f"{'['+format(100*lo,'.1f')+', '+format(100*hi,'.1f')+']':>16s}")

        # Ceiling-vs-floor: is the pin-rate gap between the two busiest
        # emitters (the experiment's two arms -- e.g. claude vs ollama) more
        # than sampling noise? Pooled two-proportion z-test on the top-2
        # sources by spawn count. The plan's stop criterion ("Claude >> Ollama
        # -> bridge is LLM-dependent") is this test, not eyeballed percentages.
        if len(per_source) >= 2:
            (sa, a), (sb, b) = sorted(
                per_source.items(), key=lambda kv: -kv[1]["spawned"])[:2]
            test = two_proportion_z_test(a["pinned"], a["spawned"],
                                         b["pinned"], b["spawned"])
            print()
            print(f"  ceiling-vs-floor (two-proportion z-test, top 2 sources):")
            ra = 100 * a["pinned"] / a["spawned"] if a["spawned"] else 0
            rb = 100 * b["pinned"] / b["spawned"] if b["spawned"] else 0
            print(f"    {sa} {ra:.1f}% (n={a['spawned']})  vs  "
                  f"{sb} {rb:.1f}% (n={b['spawned']})")
            if test is None:
                print(f"    undefined -- an arm is empty or had zero variance "
                      f"(all/none pinned on both sides).")
            else:
                z, pval = test
                if pval < 0.05:
                    print(f"    z={z:.2f}, p={pval:.3f} -- SIGNIFICANT: rates "
                          f"differ by more than sampling noise.")
                else:
                    print(f"    z={z:.2f}, p={pval:.3f} -- not significant: the "
                          f"gap is within sampling noise; don't call a winner yet.")

    print()
    print("=== phase-2 stop criteria ===")
    if not n_spawned:
        print("  no phantoms seen yet -- run the harness or pipe a real LLM")
        print("  through scripts/llm_phantom.py to generate data.")
        return
    # Judge the [5, 50]% band against the Wilson interval, not the point
    # estimate -- a stop verdict on a handful of phantoms is just noise. STOP
    # fires only when the whole interval clears a threshold; if the interval
    # straddles one, the data can't yet support the call.
    band_lo, band_hi = 0.05, 0.50
    ci = f"(95% CI {100*pin_lo:.1f}-{100*pin_hi:.1f}%, n={n_spawned})"
    if pin_lo > band_hi:
        print(f"  STOP: pin rate is confidently above 50% {ci} -- gradient isn't")
        print( "        gating; operator is pinning everything. Redesign first.")
    elif pin_hi < band_lo:
        print(f"  STOP: pin rate is confidently below 5% {ci} -- model too noisy")
        print( "        to be worth the surface area, OR the operator isn't engaging.")
    elif pin_lo >= band_lo and pin_hi <= band_hi:
        print(f"  OK:   pin rate sits inside the healthy band [5, 50]% {ci}.")
    else:
        print(f"  WAIT: point estimate {pin_rate:.0f}% but the interval straddles a")
        print(f"        band edge {ci} -- too few phantoms to call it. Keep running.")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--db", default="projects/default.db",
                   help="zoigraph project DB to read events from")
    p.add_argument("--out", default=None,
                   help="CSV output path (default: skip CSV)")
    p.add_argument("--summary", action="store_true", default=True,
                   help="print derived metrics (default on)")
    p.add_argument("--no-summary", dest="summary", action="store_false")
    args = p.parse_args(argv)

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"db not found: {args.db}", file=sys.stderr)
        return 1

    try:
        events = load_events(args.db)
    except sqlite3.OperationalError as e:
        # "no such table: events" if the DB pre-dates the phase-2 schema.
        # Don't crash -- the operator may want to know.
        print(f"could not read events from {args.db}: {e}", file=sys.stderr)
        return 2

    if args.out:
        write_csv(events, args.out)
        print(f"wrote {len(events)} events to {args.out}")
        if args.summary:
            print()

    if args.summary:
        summarize(events)
    return 0


if __name__ == "__main__":
    sys.exit(main())
