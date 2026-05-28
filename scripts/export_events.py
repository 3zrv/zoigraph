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
import sqlite3
import statistics
import sys
from collections import Counter
from pathlib import Path


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
    for e in events:
        p = e["parsed"]
        if e["kind"] == "phantom_spawn":
            pid = p.get("phantom_id")
            spawned[pid] = e["ts"]
            source_of[pid] = p.get("source") or "(unsourced)"
        elif e["kind"] == "phantom_pin":
            pinned[p.get("phantom_id")] = e["ts"]
        elif e["kind"] == "phantom_decay":
            decayed[p.get("phantom_id")] = e["ts"]

    n_spawned = len(spawned)
    pin_rate   = (100 * len(pinned)  / n_spawned) if n_spawned else 0.0
    decay_rate = (100 * len(decayed) / n_spawned) if n_spawned else 0.0

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
    print(f"  phantoms pinned:     {len(pinned):>3}   ({pin_rate:5.1f}%)")
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
              f"{'decayed':>8s} {'pin %':>7s}")
        for src in sorted(per_source.keys(),
                          key=lambda s: -per_source[s]["spawned"]):
            st = per_source[src]
            pr = (100 * st["pinned"] / st["spawned"]) if st["spawned"] else 0
            print(f"    {src:<28s} {st['spawned']:>8d} {st['pinned']:>8d} "
                  f"{st['decayed']:>8d} {pr:>6.1f}%")

    print()
    print("=== phase-2 stop criteria ===")
    if not n_spawned:
        print("  no phantoms seen yet -- run the harness or pipe a real LLM")
        print("  through scripts/llm_phantom.py to generate data.")
        return
    if pin_rate > 50.0:
        print(f"  STOP: pin_rate {pin_rate:.0f}% > 50% -- gradient isn't gating;")
        print( "        operator is pinning everything. Redesign before phase 3.")
    elif pin_rate < 5.0:
        print(f"  STOP: pin_rate {pin_rate:.0f}% < 5% -- model too noisy to be worth")
        print( "        the surface area, OR the operator isn't engaging.")
    else:
        print(f"  OK:   pin_rate {pin_rate:.0f}% in healthy band [5, 50].")


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
