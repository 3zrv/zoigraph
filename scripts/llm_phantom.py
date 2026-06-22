#!/usr/bin/env python3
"""LLM -> zoigraph phantom bridge. Phase 0 (emit) + Phase 1 (measure).

Phase 0 wiring smoke test:
    python3 scripts/llm_phantom.py emit --backend mock --db projects/default.db

Phase 1 structured-output reliability run:
    python3 scripts/llm_phantom.py measure \\
        --backend ollama --model llama3.2:3b --runs 100 \\
        --db projects/default.db --out phase1_results.json

The mock backend produces synthetic, mostly-valid JSON so the harness can be
verified end-to-end before installing Ollama. See llm_bridge.md for the full
plan and stop criteria.
"""

import argparse
import json
import math
import os
import random
import re
import socket
import sqlite3
import statistics
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Data + DB
# ---------------------------------------------------------------------------

@dataclass
class Node:
    id: int
    title: str
    content: str
    x: float
    y: float
    z: float


def load_nodes(db_path: str) -> list[Node]:
    con = sqlite3.connect(db_path)
    cur = con.cursor()
    # Soft-deleted nodes must not re-enter LLM prompts via Ask, and must not
    # count as resolved connection targets in the metrics. DBs from before
    # the tombstone column exist in the wild (pre-migration project files);
    # degrade to unfiltered rather than crashing on them.
    has_deleted = cur.execute(
        "SELECT 1 FROM pragma_table_info('nodes') WHERE name = 'deleted'"
    ).fetchone() is not None
    where = "WHERE deleted = 0 " if has_deleted else ""
    rows = cur.execute(
        f"SELECT id, title, content, x, y, z FROM nodes {where}ORDER BY id"
    ).fetchall()
    con.close()
    return [Node(r[0], r[1], r[2], r[3], r[4], r[5]) for r in rows]


# ---------------------------------------------------------------------------
# Query channel (the production context path: ask a running zoigraph instead
# of reading the DB file directly -- so we get PPR-ranked relevance, and the
# DB can be encrypted at rest later without handing a key to this process)
# ---------------------------------------------------------------------------

def read_query_token(db_path: str) -> str:
    """The session auth token zoigraph wrote next to the project DB
    (<db>.token), or "" if absent."""
    try:
        return Path(str(db_path) + ".token").read_text().strip()
    except OSError:
        return ""


def query_neighborhood(host: str, port: int, token: str, anchor_id: int,
                       hops: int, timeout: float = 5.0) -> Optional[list[Node]]:
    """Ask a running zoigraph's query channel for the anchor's relevance
    neighbourhood (personalized PageRank, anchor first). Returns the nodes, or
    None if the channel doesn't answer (app not running, bad token, unknown id,
    malformed reply) so the caller can fall back to a direct DB read."""
    req = {"q": "neighborhood", "req": 1, "id": anchor_id,
           "hops": hops, "token": token}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.bind(("127.0.0.1", 0))
        sock.sendto(json.dumps(req).encode(), (host, port))
        data, _ = sock.recvfrom(65536)
    except (socket.timeout, OSError):
        return None
    finally:
        sock.close()
    try:
        reply = json.loads(data.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None
    if not isinstance(reply, dict) or "error" in reply:
        return None
    out = []
    for n in reply.get("nodes", []):
        out.append(Node(n["id"], n.get("title", ""), n.get("content", ""),
                        float(n.get("x", 0.0)), float(n.get("y", 0.0)),
                        float(n.get("z", 0.0))))
    return out or None


# ---------------------------------------------------------------------------
# Prompt + context selection
# ---------------------------------------------------------------------------

PROMPT_TEMPLATE = """You are proposing a new entity to connect existing entities in a knowledge graph.

Existing entities (id, title, position, content snippet):
{context_block}

Propose ONE new entity that meaningfully connects 2 or 3 of these. Output ONLY a single JSON object, no prose, no markdown, no code fences. The object MUST have exactly these keys:

{{"id": <integer between 5000 and 9999>, "x": <float>, "y": <float>, "z": <float>, "label": "<short label, <= 40 chars>", "content": "<one or two sentences explaining the connection, <= 240 chars>", "connections": [{{"target": <id of an existing entity above>, "kind": "<relationship type>"}}, ...]}}

Rules:
- "id" must NOT collide with any existing id shown above.
- "connections" must be a non-empty array of 2 or 3 objects. Each object's "target" is one of the existing entity ids shown above.
- "kind" is the relationship type from the closed set: "knows", "owns", "saw-at", "shell-of", "part-of", "works-at", "created", "influenced", "wrote", "predecessor-of", or "" if no kind fits.
- The (x, y, z) MUST be near the centroid of the entities listed in "connections" -- within roughly 5 units in each axis.
- "label" is a short noun phrase. No quotes inside the string. No newlines.
- "content" is one or two sentences explaining WHY this new entity connects the listed existing entities. Plain text. No quotes inside. No newlines.

Output the JSON object now:"""


def build_context_block(context: list[Node]) -> str:
    lines = []
    for n in context:
        snippet = (n.content or "").replace("\n", " ")[:60]
        lines.append(
            f"- id={n.id}: \"{n.title}\" "
            f"(x={n.x:.1f}, y={n.y:.1f}, z={n.z:.1f}) -- {snippet}"
        )
    return "\n".join(lines)


def build_prompt(context: list[Node]) -> str:
    return PROMPT_TEMPLATE.format(context_block=build_context_block(context))


def pick_context(
    all_nodes: list[Node],
    anchor: Node,
    k: int,
    rng: random.Random,
) -> list[Node]:
    """Anchor + k-1 spatial neighbours of anchor (so the LLM sees a locally
    coherent slice, not a random sprinkle across the whole graph)."""
    others = [n for n in all_nodes if n.id != anchor.id]
    others.sort(
        key=lambda n: (n.x - anchor.x) ** 2
        + (n.y - anchor.y) ** 2
        + (n.z - anchor.z) ** 2
    )
    near = others[: max(k * 3, 12)]
    rng.shuffle(near)
    return [anchor] + near[: k - 1]


# ---------------------------------------------------------------------------
# Backends
# ---------------------------------------------------------------------------

class Backend:
    name = "abstract"

    def generate(self, prompt: str) -> str:
        raise NotImplementedError


class MockBackend(Backend):
    """Deterministic-ish backend that simulates a structured-output model.

    Failure modes injected, in proportion to --failure-rate:
      - emits prose before the JSON (recoverable by the parser if it's lenient)
      - emits invalid JSON
      - omits a required key
      - places (x,y,z) far from the connection centroid
      - invents an id not in the context
    """

    name = "mock"

    def __init__(self, failure_rate: float, rng: random.Random):
        self.failure_rate = failure_rate
        self.rng = rng

    def generate(self, prompt: str) -> str:
        ids = [int(x) for x in re.findall(r"id=(\d+)", prompt)]
        positions = [
            (float(m[0]), float(m[1]), float(m[2]))
            for m in re.findall(
                r"x=(-?\d+\.\d+), y=(-?\d+\.\d+), z=(-?\d+\.\d+)", prompt
            )
        ]
        if not ids:
            return "{}"

        roll = self.rng.random()

        # ~5% emit pure invalid JSON
        if roll < self.failure_rate * 0.5:
            return "this is not json, sorry"

        # pick 2-3 connections
        n_conn = self.rng.randint(2, min(3, len(ids)))
        chosen_idx = self.rng.sample(range(len(ids)), n_conn)
        chosen_ids = [ids[i] for i in chosen_idx]
        cx = sum(positions[i][0] for i in chosen_idx) / n_conn
        cy = sum(positions[i][1] for i in chosen_idx) / n_conn
        cz = sum(positions[i][2] for i in chosen_idx) / n_conn

        # ~3% wildly misplaced
        if roll < self.failure_rate * 0.8:
            cx += self.rng.uniform(-200, 200)
            cy += self.rng.uniform(-200, 200)
            cz += self.rng.uniform(-200, 200)

        # ~2% invent an id
        if roll < self.failure_rate:
            chosen_ids[0] = 12345

        kinds = ["knows", "shell-of", "saw-at", "part-of", "works-at", ""]
        obj = {
            "id": self.rng.randint(5000, 9999),
            "x": round(cx + self.rng.uniform(-2, 2), 2),
            "y": round(cy + self.rng.uniform(-2, 2), 2),
            "z": round(cz + self.rng.uniform(-2, 2), 2),
            "label": f"mock-hypothesis-{self.rng.randint(0, 999)}",
            "content": f"mock reasoning {self.rng.randint(0, 999)}",
            "connections": [
                {"target": cid, "kind": self.rng.choice(kinds)}
                for cid in chosen_ids
            ],
        }
        # ~2% omit a key
        if self.rng.random() < self.failure_rate * 0.5:
            del obj["label"]

        return json.dumps(obj)


class OllamaBackend(Backend):
    """Calls a local Ollama daemon via HTTP. Requires `ollama serve` running
    and the chosen model pulled. The format=json hint pushes models toward
    structured output but doesn't enforce a schema -- that's what we're
    measuring."""

    name = "ollama"

    def __init__(self, model: str, host: str = "http://localhost:11434",
                 timeout: float = 60.0):
        self.model = model
        self.host = host.rstrip("/")
        self.timeout = timeout

    def generate(self, prompt: str) -> str:
        body = json.dumps({
            "model": self.model,
            "prompt": prompt,
            "format": "json",
            "stream": False,
            "options": {"temperature": 0.6, "num_predict": 256},
        }).encode("utf-8")
        req = urllib.request.Request(
            f"{self.host}/api/generate",
            data=body,
            headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                payload = json.loads(r.read().decode("utf-8"))
        except urllib.error.URLError as e:
            raise RuntimeError(
                f"Ollama HTTP error at {self.host}: {e}. Is `ollama serve` "
                f"running and `{self.model}` pulled?"
            ) from e
        return payload.get("response", "")


class ClaudeBackend(Backend):
    """Calls the Anthropic Messages API. This is the phase-2 *ceiling*
    backend (llm_bridge.md): if the trust gradient holds against the
    strongest, most articulate output, it really holds. Requires
    ANTHROPIC_API_KEY in the environment.

    JSON enforcement uses the API's structured-output mode
    (output_config.format with a JSON schema) -- the native equivalent
    of Ollama's format=json hint. Assistant prefill, the old trick for
    forcing a bare JSON object, returns 400 on current models. No
    temperature either: sampling params are removed on Opus 4.7+ (also
    400) -- an inherent asymmetry vs the ollama backend's 0.6, worth
    remembering when comparing duplicate-suggestion rates across
    backends."""

    name = "claude"

    # Mirrors REQUIRED_KEYS + the connection shape parse_phantom
    # validates. The API enforces this server-side, so valid_schema_pct
    # should be ~100 by construction -- the interesting phase-2 signal
    # for this backend is pin rate, not JSON validity.
    OUTPUT_SCHEMA = {
        "type": "object",
        "properties": {
            "id":      {"type": "integer"},
            "x":       {"type": "number"},
            "y":       {"type": "number"},
            "z":       {"type": "number"},
            "label":   {"type": "string"},
            "content": {"type": "string"},
            "connections": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "target": {"type": "integer"},
                        "kind":   {"type": "string"},
                    },
                    "required": ["target", "kind"],
                    "additionalProperties": False,
                },
            },
        },
        "required": ["id", "x", "y", "z", "label", "content", "connections"],
        "additionalProperties": False,
    }

    def __init__(self, model: str, timeout: float = 60.0):
        self.model = model
        self.timeout = timeout
        self.api_key = os.environ.get("ANTHROPIC_API_KEY", "")

    def generate(self, prompt: str) -> str:
        if not self.api_key:
            raise RuntimeError(
                "ANTHROPIC_API_KEY is not set -- the claude backend needs "
                "it. export ANTHROPIC_API_KEY=... and re-run.")
        body = json.dumps({
            "model": self.model,
            "max_tokens": 1024,
            "output_config": {
                "format": {"type": "json_schema",
                           "schema": self.OUTPUT_SCHEMA},
            },
            "messages": [{"role": "user", "content": prompt}],
        }).encode("utf-8")
        req = urllib.request.Request(
            "https://api.anthropic.com/v1/messages",
            data=body,
            headers={
                "Content-Type":      "application/json",
                "x-api-key":         self.api_key,
                "anthropic-version": "2023-06-01",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                payload = json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                detail = e.read().decode("utf-8")[:300]
            except Exception:
                detail = ""
            raise RuntimeError(
                f"Anthropic API HTTP {e.code}: {detail}") from e
        except urllib.error.URLError as e:
            raise RuntimeError(f"Anthropic API unreachable: {e}") from e
        return "".join(
            b.get("text", "") for b in payload.get("content", [])
            if b.get("type") == "text")


# Per-backend model defaults, applied when --model is omitted.
DEFAULT_MODELS = {
    "ollama": "llama3.2:3b",
    "claude": "claude-opus-4-8",
}


def make_backend(name: str, model: Optional[str], failure_rate: float,
                 rng: random.Random) -> Backend:
    model = model or DEFAULT_MODELS.get(name, "")
    if name == "mock":
        return MockBackend(failure_rate=failure_rate, rng=rng)
    if name == "ollama":
        return OllamaBackend(model=model)
    if name == "claude":
        # Sensitivity rule (llm_bridge.md): the remote backend never sees
        # real threat-model content -- benign corpora only.
        print("NOTE: claude is a REMOTE backend -- benign corpora only "
              "(llm_bridge.md sensitivity rule).", file=sys.stderr)
        return ClaudeBackend(model=model)
    raise SystemExit(f"unknown backend: {name}")


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

REQUIRED_KEYS = {"id", "x", "y", "z", "label", "connections"}


def parse_phantom(raw: str) -> dict:
    """Returns a dict with keys: parsed_ok, schema_ok, data, error.
    `data` is the parsed object when parsed_ok, else None."""
    out = {"parsed_ok": False, "schema_ok": False, "data": None, "error": ""}
    s = raw.strip()
    # Strip markdown fences if any.
    s = re.sub(r"^```(?:json)?\s*", "", s)
    s = re.sub(r"\s*```$", "", s)

    obj = None
    try:
        obj = json.loads(s)
    except json.JSONDecodeError:
        # Try to extract the first {...} block. Models often wrap with prose.
        m = re.search(r"\{.*\}", s, flags=re.DOTALL)
        if m:
            try:
                obj = json.loads(m.group(0))
            except json.JSONDecodeError as e:
                out["error"] = f"json_decode: {e}"
                return out
        else:
            out["error"] = "no_json_object_found"
            return out

    out["parsed_ok"] = True
    out["data"] = obj

    if not isinstance(obj, dict):
        out["error"] = "not_an_object"
        return out

    missing = REQUIRED_KEYS - set(obj.keys())
    if missing:
        out["error"] = f"missing_keys: {sorted(missing)}"
        return out

    if not isinstance(obj["id"], int):
        out["error"] = "id_not_int"
        return out
    for axis in ("x", "y", "z"):
        if not isinstance(obj[axis], (int, float)):
            out["error"] = f"{axis}_not_number"
            return out
    if not isinstance(obj["label"], str):
        out["error"] = "label_not_string"
        return out
    if not isinstance(obj["connections"], list) or not obj["connections"]:
        out["error"] = "connections_not_nonempty_list"
        return out
    # Accept both shapes per element to match the C++ parser:
    #   - bare int                          (legacy)
    #   - {"target": int, "kind": <str>}    (new, with kind)
    # Anything else fails validation -- a single bad entry rejects the
    # whole response so the harness's filler/grounded metrics aren't
    # polluted by half-valid payloads.
    for c in obj["connections"]:
        if isinstance(c, int):
            continue
        if (isinstance(c, dict)
                and "target" in c and isinstance(c["target"], int)):
            if "kind" in c and not isinstance(c["kind"], str):
                out["error"] = "connection_kind_not_string"
                return out
            continue
        out["error"] = "connection_member_not_int_or_object"
        return out

    out["schema_ok"] = True
    return out


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

# Tokens that describe the *shape* of the request rather than the *content*
# of the entities being connected. Accumulated from observed Phase 1 runs:
# llama3.2:3b favoured "cluster/centroid/outlier"; qwen2.5:7b-instruct
# favoured "junction/convergence/hub-connector". The set is a running log
# of known filler vocabulary across models; extend it as new models surface
# new failure patterns. Matched case-insensitively against any TOKEN in the
# label, not the whole string — so "junction-xyz" and "hub-connector" both
# trip on "junction" / "hub" / "connector".
FILLER_TOKENS = {
    # geometric / topological
    "cluster", "centroid", "outlier", "anomaly", "hub", "intersection",
    "junction", "convergence", "nexus", "center", "centre", "point",
    "core", "midpoint", "vertex", "pivot", "fulcrum",
    # structural role
    "node", "entity", "group", "connection", "connector", "link",
    "edge", "site", "object", "subject", "thing", "item",
    # generic event / agent words the models reach for
    "encounter", "incident", "observer", "interceptor", "phantom",
    "correlation", "incidence", "pattern", "matcher",
    # placeholder slots emitted as if this were a template-fill task
    "xyz", "abc", "placeholder", "tbd", "todo",
}

_TOKEN_RE = re.compile(r"[a-z]{3,}")
_PLACEHOLDER_SUFFIX_RE = re.compile(r"-(?:xyz|abc|\d{2,})$")


def _tokens(s: str) -> set[str]:
    return set(_TOKEN_RE.findall(s.lower()))


def _label_is_filler(label: str) -> bool:
    """True if any token in the label is in the known-filler set, OR if the
    label ends in a placeholder-shaped suffix like '-xyz' / '-5042'. We
    treat token-level rather than whole-string match because models compose
    filler ('hub-connector', 'junction-point') around the same root."""
    lab = label.strip().lower()
    if _PLACEHOLDER_SUFFIX_RE.search(lab):
        return True
    return bool(_tokens(lab) & FILLER_TOKENS)


def _label_grounded(label: str, connection_titles: list[str]) -> bool:
    """True iff the label shares any >=3-char alpha token with the titles of
    the nodes it claims to connect. Crude but interpretable measure of
    semantic engagement — model borrowed vocabulary from the actual entities
    rather than inventing schema-shape filler."""
    lt = _tokens(label)
    if not lt:
        return False
    tt = set()
    for t in connection_titles:
        tt |= _tokens(t)
    return bool(lt & tt)


def _connection_target(c) -> Optional[int]:
    """Extract the target id from either shape of connection entry."""
    if isinstance(c, int):
        return c
    if isinstance(c, dict) and isinstance(c.get("target"), int):
        return c["target"]
    return None


def compute_metrics(results: list[dict], all_nodes: list[Node]) -> dict:
    id_to_pos = {n.id: (n.x, n.y, n.z) for n in all_nodes}
    id_to_title = {n.id: n.title for n in all_nodes}
    n = len(results)
    parsed = sum(1 for r in results if r["parse"]["parsed_ok"])
    schema = sum(1 for r in results if r["parse"]["schema_ok"])

    # connection resolution + centroid distance, only on schema-valid
    resolution_pcts = []
    centroid_dists = []
    centroid_dists_norm = []
    labels = []
    conn_sets = []
    filler_hits = 0
    grounded_hits = 0
    for r in results:
        if not r["parse"]["schema_ok"]:
            continue
        data = r["parse"]["data"]
        # Normalise to a plain list of target ids; downstream stats don't
        # use kind. Skip malformed entries.
        conn_targets = [t for t in
                        (_connection_target(c) for c in data["connections"])
                        if t is not None]
        resolved = [c for c in conn_targets if c in id_to_pos]
        resolution_pcts.append(
            len(resolved) / len(conn_targets) if conn_targets else 0.0)

        if resolved:
            cx = sum(id_to_pos[c][0] for c in resolved) / len(resolved)
            cy = sum(id_to_pos[c][1] for c in resolved) / len(resolved)
            cz = sum(id_to_pos[c][2] for c in resolved) / len(resolved)
            dx, dy, dz = data["x"] - cx, data["y"] - cy, data["z"] - cz
            dist = math.sqrt(dx * dx + dy * dy + dz * dz)
            centroid_dists.append(dist)

            # Normalize by mean pairwise distance among resolved connections.
            # That tells us "did the model place it inside the cluster" (norm
            # < 1) vs "off in space somewhere" (norm >> 1).
            if len(resolved) >= 2:
                pairs = []
                for i in range(len(resolved)):
                    for j in range(i + 1, len(resolved)):
                        a, b = id_to_pos[resolved[i]], id_to_pos[resolved[j]]
                        pairs.append(math.sqrt(
                            (a[0] - b[0]) ** 2
                            + (a[1] - b[1]) ** 2
                            + (a[2] - b[2]) ** 2
                        ))
                mean_pair = statistics.mean(pairs) if pairs else 1.0
                if mean_pair > 0:
                    centroid_dists_norm.append(dist / mean_pair)

        labels.append(data["label"])
        conn_sets.append(tuple(sorted(conn_targets)))

        if _label_is_filler(data["label"]):
            filler_hits += 1
        conn_titles = [id_to_title[c] for c in resolved if c in id_to_title]
        if _label_grounded(data["label"], conn_titles):
            grounded_hits += 1

    unique_labels = len(set(labels)) if labels else 0
    unique_conn_sets = len(set(conn_sets)) if conn_sets else 0
    n_schema = len(labels)

    def median_or_none(xs):
        return statistics.median(xs) if xs else None

    return {
        "n_runs": n,
        "valid_json_pct": round(100 * parsed / n, 1) if n else 0.0,
        "valid_schema_pct": round(100 * schema / n, 1) if n else 0.0,
        "connection_resolution_pct": (
            round(100 * statistics.mean(resolution_pcts), 1)
            if resolution_pcts else None
        ),
        "centroid_distance_median": (
            round(median_or_none(centroid_dists), 2)
            if centroid_dists else None
        ),
        "centroid_distance_normalized_median": (
            round(median_or_none(centroid_dists_norm), 3)
            if centroid_dists_norm else None
        ),
        "unique_labels_pct": (
            round(100 * unique_labels / len(labels), 1) if labels else None
        ),
        "unique_connection_sets_pct": (
            round(100 * unique_conn_sets / len(conn_sets), 1)
            if conn_sets else None
        ),
        # Semantic engagement — how often the label is generic schema-shape
        # filler vs. drawn from the actual entities' vocabulary. These two
        # are not complements (a label can be neither filler nor grounded —
        # e.g. a novel but generic term like "encounter").
        "filler_label_pct": (
            round(100 * filler_hits / n_schema, 1) if n_schema else None
        ),
        "label_grounded_pct": (
            round(100 * grounded_hits / n_schema, 1) if n_schema else None
        ),
        # Phase 1 hard stop: <80% valid JSON
        "stop_phase_1": (parsed / n) < 0.8 if n else None,
    }


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------

def _source_tag(backend_name: str, model: str) -> str:
    """Stable identifier for the emitting backend, embedded in every phantom
    payload so the trust-gradient analysis can compare pin rates by source.
    Mock backend tagged just `mock` since model is irrelevant there."""
    if backend_name == "mock":
        return "mock"
    return f"{backend_name}:{model}"


def cmd_emit(args: argparse.Namespace) -> int:
    rng = random.Random(args.seed)
    context = None
    # Production path: ask a running zoigraph for the anchor's relevance
    # neighbourhood over the query channel. No direct DB read (keeps the
    # SQLCipher swap-in clean), and the context is PPR-ranked, not spatial.
    if args.anchor_id is not None and not args.no_channel:
        token = read_query_token(args.db)
        if not token:
            print(f"no query token at {args.db}.token -- falling back to DB read",
                  file=sys.stderr)
        else:
            ctx = query_neighborhood(args.host, args.query_port, token,
                                     args.anchor_id, args.hops)
            if ctx:
                context = ctx
                print(f"=== context via query channel: {len(context)} nodes "
                      f"(anchor id={context[0].id}) ===")
            else:
                print("query channel unreachable/empty -- falling back to DB read",
                      file=sys.stderr)
    if context is None:
        # Fallback (app not running / --no-channel / no anchor): read the DB
        # and pick a spatial slice, as the phase-1 measure harness does.
        nodes = load_nodes(args.db)
        if len(nodes) < args.k:
            print(f"need at least {args.k} nodes in {args.db}", file=sys.stderr)
            return 2
        if args.anchor_id is not None:
            # Fail loudly on a bad id so the C++ side surfaces a useful error.
            match = [n for n in nodes if n.id == args.anchor_id]
            if not match:
                print(f"--anchor-id {args.anchor_id} not found in {args.db}",
                      file=sys.stderr)
                return 4
            anchor = match[0]
        else:
            anchor = rng.choice(nodes)
        context = pick_context(nodes, anchor, args.k, rng)
    prompt = build_prompt(context)
    backend = make_backend(args.backend, args.model, args.failure_rate, rng)
    raw = backend.generate(prompt)
    parsed = parse_phantom(raw)
    print("=== raw response ===")
    print(raw)
    print("=== parse ===")
    print(json.dumps({k: v for k, v in parsed.items() if k != "data"},
                     indent=2))
    if not parsed["schema_ok"]:
        print("schema invalid -- not sending UDP packet", file=sys.stderr)
        return 1
    data = parsed["data"]
    # The model proposes an id (and schema validation checks it), but we do
    # NOT trust it for uniqueness on the wire: [5000, 9999] collides with
    # ~98% probability over 200 emissions (birthday bound), and both the
    # C++ spawn-tracker and export_events.py key phantom lifecycle by this
    # id. Mint a locally-unique one instead -- epoch microseconds. Fits
    # Phantom::id (int64); disjoint from the model range and the toolbar's
    # local-inject counter (1'000'000+); same-microsecond emissions from
    # this one-packet-per-process script are practically impossible.
    data["id"] = int(time.time() * 1_000_000)
    source = args.source or _source_tag(
        args.backend, getattr(backend, "model", None) or args.model or "")
    # Normalise connections to the object shape on the way out so the
    # listener and the events table see a uniform schema regardless of
    # what the LLM happened to emit.
    norm_conns = []
    for c in data["connections"]:
        if isinstance(c, int):
            norm_conns.append({"target": c, "kind": ""})
        elif isinstance(c, dict):
            norm_conns.append({
                "target": c["target"],
                "kind": c.get("kind", "") if isinstance(c.get("kind"), str) else "",
            })
    # Place the phantom at the centroid of the referents the model actually
    # chose (positions come from the context we supplied), overriding its
    # guessed (x,y,z) so the phantom always materialises among the nodes it
    # connects -- more reliable than trusting the model's spatial reasoning.
    pos_by_id = {n.id: (n.x, n.y, n.z) for n in context}
    pts = [pos_by_id[c["target"]] for c in norm_conns if c["target"] in pos_by_id]
    if pts:
        data["x"] = sum(p[0] for p in pts) / len(pts)
        data["y"] = sum(p[1] for p in pts) / len(pts)
        data["z"] = sum(p[2] for p in pts) / len(pts)
    payload = json.dumps({
        "id":          data["id"],
        "x":           data["x"],
        "y":           data["y"],
        "z":           data["z"],
        "label":       data["label"],
        "connections": norm_conns,
        "source":      source,
        "content":     data.get("content", ""),
        # Owning-project tag (derived from the --db filename): the app
        # drops phantoms tagged for a project other than the active one,
        # so an Ask that lands after a project switch can't pin wrong-
        # graph edges or pollute the wrong events table.
        "project":     Path(args.db).stem,
    }).encode("utf-8")
    # The UDP listener caps datagrams at 1 KiB (kMaxPayload) and silently drops
    # anything larger, so sending an oversized payload would print "done" while
    # the phantom never appears. Fail loudly instead.
    if len(payload) > 1024:
        print(f"payload is {len(payload)} bytes, over the listener's 1024-byte "
              f"cap -- not sending (shorten the label/content)", file=sys.stderr)
        return 6
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(payload, (args.host, args.port))
    sock.close()
    print(f"=== sent {len(payload)} bytes to {args.host}:{args.port} "
          f"(source={source}) ===")
    return 0


def cmd_measure(args: argparse.Namespace) -> int:
    rng = random.Random(args.seed)
    nodes = load_nodes(args.db)
    if len(nodes) < args.k:
        print(f"need at least {args.k} nodes in {args.db}", file=sys.stderr)
        return 2
    backend = make_backend(args.backend, args.model, args.failure_rate, rng)

    results = []
    t_start = time.time()
    for i in range(args.runs):
        anchor = rng.choice(nodes)
        context = pick_context(nodes, anchor, args.k, rng)
        prompt = build_prompt(context)
        t0 = time.time()
        try:
            raw = backend.generate(prompt)
            err = ""
        except Exception as e:
            raw = ""
            err = repr(e)
        dt = time.time() - t0
        parsed = parse_phantom(raw) if raw else {
            "parsed_ok": False, "schema_ok": False,
            "data": None, "error": f"backend_error: {err}",
        }
        results.append({
            "i": i,
            "anchor_id": anchor.id,
            "context_ids": [n.id for n in context],
            "elapsed_s": round(dt, 3),
            "raw": raw,
            "parse": parsed,
        })
        if (i + 1) % 10 == 0 or i == args.runs - 1:
            print(f"  [{i + 1}/{args.runs}] "
                  f"parsed={sum(1 for r in results if r['parse']['parsed_ok'])} "
                  f"schema={sum(1 for r in results if r['parse']['schema_ok'])}",
                  file=sys.stderr)

    metrics = compute_metrics(results, nodes)
    metrics["wall_s"] = round(time.time() - t_start, 2)
    metrics["backend"] = backend.name
    metrics["model"] = getattr(backend, "model", None)
    metrics["k"] = args.k

    Path(args.out).write_text(
        json.dumps({
            "metrics": metrics,
            # Snapshot of the graph state the run was scored against.
            # save_graph rewrites x/y/z on every save/shutdown and the
            # operator edits titles, so rescore MUST NOT re-read the live
            # DB: centroid_distance_* depends on positions and
            # label_grounded_pct on titles as they were at run time.
            "node_snapshot": {
                str(n.id): {"title": n.title, "pos": [n.x, n.y, n.z]}
                for n in nodes
            },
            "results": results,
        }, indent=2)
    )

    print()
    print("=== Phase 1 metrics ===")
    for k, v in metrics.items():
        print(f"  {k:42s} {v}")
    print()
    print(f"raw responses + parse details: {args.out}")
    if metrics["stop_phase_1"]:
        print("STOP: <80% valid JSON. Fix structured-output before phase 2.",
              file=sys.stderr)
        return 3
    return 0


def cmd_rescore(args: argparse.Namespace) -> int:
    """Recompute metrics on a previously-saved results.json. Cheap — no LLM
    call. Use this after extending compute_metrics so old runs can be
    compared apples-to-apples with new ones."""
    data = json.loads(Path(args.input).read_text())
    snap = data.get("node_snapshot")
    if snap is not None:
        # Preferred path: score against the graph as it was at run time.
        nodes = [Node(int(i), s["title"], "", s["pos"][0], s["pos"][1],
                      s["pos"][2])
                 for i, s in snap.items()]
    elif args.allow_drift:
        print("WARNING: no node_snapshot in this results file; scoring "
              "against the CURRENT DB. Positions are rewritten on every "
              "save and titles drift with edits, so centroid_distance_* "
              "and label_grounded_pct may not match the original run.",
              file=sys.stderr)
        nodes = load_nodes(args.db)
    else:
        print(f"{args.input} predates node snapshots: positions/titles in "
              f"the live DB have likely drifted since the run, which "
              f"silently skews centroid + grounded metrics. Re-run "
              f"`measure`, or pass --allow-drift to score against the "
              f"current DB anyway.", file=sys.stderr)
        return 5
    results = data["results"]
    old_metrics = data.get("metrics", {})
    new_metrics = compute_metrics(results, nodes)
    # Preserve metadata from the original run.
    for k in ("wall_s", "backend", "model", "k"):
        if k in old_metrics:
            new_metrics[k] = old_metrics[k]

    print(f"=== rescored: {args.input} ===")
    print(f"{'metric':42s} {'old':>10s}  {'new':>10s}")
    keys = list(new_metrics.keys())
    for k in keys:
        old_v = old_metrics.get(k, "—")
        new_v = new_metrics[k]
        marker = "" if old_v == new_v else "  *"
        print(f"  {k:40s} {str(old_v):>10s}  {str(new_v):>10s}{marker}")

    if args.out:
        Path(args.out).write_text(
            json.dumps({"metrics": new_metrics, "results": results}, indent=2)
        )
        print(f"\nwrote: {args.out}")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--db", default="projects/default.db",
                        help="zoigraph project DB to sample nodes from")
    common.add_argument("--backend", choices=["mock", "ollama", "claude"],
                        default="mock")
    common.add_argument("--model", default=None,
                        help="model tag; default depends on backend "
                             "(ollama: llama3.2:3b, claude: claude-opus-4-8; "
                             "ignored by mock)")
    common.add_argument("--k", type=int, default=5,
                        help="anchor + k-1 spatial neighbours per prompt")
    common.add_argument("--seed", type=int, default=1)
    common.add_argument("--failure-rate", type=float, default=0.10,
                        help="mock backend failure injection rate")

    pe = sub.add_parser("emit", parents=[common],
                        help="Phase 0 -- generate one phantom and send to UDP")
    pe.add_argument("--host", default="127.0.0.1")
    pe.add_argument("--port", type=int, default=7777)
    pe.add_argument("--source", default=None,
                    help="override the source tag embedded in the payload "
                         "(default: '<backend>:<model>')")
    pe.add_argument("--anchor-id", type=int, default=None,
                    help="pin the prompt anchor to a specific node id "
                         "(default: random). used by the in-app Ask button.")
    pe.add_argument("--query-port", type=int, default=7778,
                    help="port of the running zoigraph's read query channel "
                         "(default: 7778). context is fetched from it when "
                         "--anchor-id is given.")
    pe.add_argument("--hops", type=int, default=2,
                    help="neighbourhood size hint for the query channel "
                         "(scales how many relevant nodes come back)")
    pe.add_argument("--no-channel", action="store_true",
                    help="skip the query channel and read context straight "
                         "from the DB (for standalone emit without a running app)")
    pe.set_defaults(func=cmd_emit)

    pm = sub.add_parser("measure", parents=[common],
                        help="Phase 1 -- run N generations, report metrics")
    pm.add_argument("--runs", type=int, default=100)
    pm.add_argument("--out", default="phase1_results.json")
    pm.set_defaults(func=cmd_measure)

    pr = sub.add_parser("rescore",
                        help="recompute metrics on a saved results.json")
    pr.add_argument("--input", required=True,
                    help="path to a previous results.json")
    pr.add_argument("--db", default="projects/default.db",
                    help="fallback DB for snapshot-less results files "
                         "(only used with --allow-drift)")
    pr.add_argument("--allow-drift", action="store_true",
                    help="permit scoring a snapshot-less results file "
                         "against the current DB despite position/title "
                         "drift since the run")
    pr.add_argument("--out", default=None,
                    help="optional rewrite path; if omitted, only prints")
    pr.set_defaults(func=cmd_rescore)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
