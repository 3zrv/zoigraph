#!/usr/bin/env python3
"""Seed a zoigraph project DB with a UNIX-history corpus.

Used by Phase 1 path (a): re-run the structured-output harness against a
corpus with *real* semantic content so the label_grounded_pct metric
isn't biased to zero by synthetic identifier titles.

Schema matches src/persistence/db.cpp's kSchema so the resulting DB also
opens cleanly in zoigraph itself.

Usage:
    python3 scripts/seed_corpus_unix.py
    python3 scripts/seed_corpus_unix.py --out projects/corpus_unix.db
"""

import argparse
import math
import random
import sqlite3
import sys
from pathlib import Path


SCHEMA = """
CREATE TABLE IF NOT EXISTS nodes (
    id           INTEGER PRIMARY KEY,
    x            REAL    NOT NULL,
    y            REAL    NOT NULL,
    z            REAL    NOT NULL,
    title        TEXT    NOT NULL DEFAULT '',
    content      TEXT    NOT NULL DEFAULT '',
    first_seen   REAL    NOT NULL DEFAULT 0.0,
    last_touched REAL    NOT NULL DEFAULT 0.0,
    tier         TEXT    NOT NULL DEFAULT 'confirmed',
    deleted      INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS edges (
    source    INTEGER NOT NULL,
    target    INTEGER NOT NULL,
    weight    REAL    NOT NULL DEFAULT 1.0,
    label     TEXT    NOT NULL DEFAULT '',
    kind      TEXT    NOT NULL DEFAULT '',
    certainty TEXT    NOT NULL DEFAULT 'confirmed'
);
CREATE TABLE IF NOT EXISTS node_tags (
    node_id INTEGER NOT NULL,
    tag     TEXT    NOT NULL,
    PRIMARY KEY (node_id, tag)
);
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
    ts      REAL    NOT NULL,
    kind    TEXT    NOT NULL,
    node_id INTEGER,
    payload TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS events_ts_idx   ON events (ts);
CREATE INDEX IF NOT EXISTS events_kind_idx ON events (kind);
CREATE VIRTUAL TABLE IF NOT EXISTS nodes_fts USING fts5(
    title,
    content,
    content='nodes',
    content_rowid='id'
);
CREATE TRIGGER IF NOT EXISTS nodes_ai AFTER INSERT ON nodes BEGIN
    INSERT INTO nodes_fts(rowid, title, content)
        VALUES (new.id, new.title, new.content);
END;
CREATE TRIGGER IF NOT EXISTS nodes_ad AFTER DELETE ON nodes BEGIN
    INSERT INTO nodes_fts(nodes_fts, rowid, title, content)
        VALUES ('delete', old.id, old.title, old.content);
END;
CREATE TRIGGER IF NOT EXISTS nodes_au AFTER UPDATE ON nodes BEGIN
    INSERT INTO nodes_fts(nodes_fts, rowid, title, content)
        VALUES ('delete', old.id, old.title, old.content);
    INSERT INTO nodes_fts(rowid, title, content)
        VALUES (new.id, new.title, new.content);
END;
"""


# Hand-curated. Each entry: (title, content, tags). Content stays short --
# what we care about is whether the LLM can pick up enough vocabulary to
# produce a label that actually engages with the entities, not whether the
# content is encyclopedic.
NODES: list[tuple[str, str, list[str]]] = [
    # --- people ---
    ("Ken Thompson",
     "Bell Labs researcher; co-creator of UNIX, the B language, and the "
     "regex implementation used in ed and grep. Authored UTF-8 with Pike.",
     ["person", "bell-labs"]),
    ("Dennis Ritchie",
     "Bell Labs; created the C programming language and co-authored UNIX. "
     "Coauthor of K&R with Kernighan. Turing Award 1983.",
     ["person", "bell-labs"]),
    ("Brian Kernighan",
     "Bell Labs; coined the name UNIX, co-wrote AWK and the K&R C book. "
     "Wrote The Unix Programming Environment with Pike.",
     ["person", "bell-labs"]),
    ("Doug McIlroy",
     "Bell Labs; invented UNIX pipes and championed the 'do one thing well' "
     "design philosophy. Manager of the UNIX research group.",
     ["person", "bell-labs"]),
    ("Rob Pike",
     "Bell Labs; co-author of UTF-8, Plan 9, and Go. Worked closely with "
     "Thompson on later UNIX research.",
     ["person", "bell-labs"]),
    ("Bill Joy",
     "UC Berkeley grad student; principal author of vi, csh, and large "
     "parts of BSD networking. Co-founded Sun Microsystems.",
     ["person", "berkeley", "sun"]),
    ("Linus Torvalds",
     "Finnish student at University of Helsinki who started Linux in 1991 "
     "as a hobby project inspired by MINIX. Also created Git.",
     ["person"]),
    ("Richard Stallman",
     "MIT AI Lab; founded the GNU Project and the Free Software Foundation. "
     "Wrote the GNU General Public License and the original GNU Emacs.",
     ["person", "mit", "fsf"]),
    ("Andrew Tanenbaum",
     "VU Amsterdam professor; created MINIX as a teaching OS. Author of "
     "the textbook that Torvalds learned operating systems from.",
     ["person"]),
    ("Eric Raymond",
     "Author of 'The Cathedral and the Bazaar' and maintainer of the "
     "Jargon File. Coined 'open source' as a marketable rebranding.",
     ["person"]),
    ("Theo de Raadt",
     "Founded OpenBSD after being removed from the NetBSD core team. "
     "Drives the project's security-first reputation.",
     ["person", "bsd"]),
    ("Bjarne Stroustrup",
     "Bell Labs; designed and implemented C++ as 'C with Classes' starting "
     "in 1979. Author of The C++ Programming Language.",
     ["person", "bell-labs"]),
    ("Larry Wall",
     "Created Perl in 1987 while at Unisys. Linguistics background; "
     "described Perl as 'practical extraction and report language.'",
     ["person"]),
    ("Donald Knuth",
     "Stanford; author of The Art of Computer Programming and creator of "
     "TeX. Coined 'literate programming.'",
     ["person", "academic"]),
    ("Edsger Dijkstra",
     "Algorithm theorist; wrote the famous 'GOTO considered harmful' "
     "letter and developed structured programming principles.",
     ["person", "academic"]),

    # --- organizations ---
    ("Bell Labs",
     "AT&T research division in Murray Hill, NJ where UNIX, C, and the "
     "transistor were invented. Site of the original UNIX research group.",
     ["org", "bell-labs"]),
    ("AT&T",
     "Owned Bell Labs and UNIX through divestiture in 1984. Spun off "
     "Unix System Laboratories (USL) which licensed System V.",
     ["org"]),
    ("UC Berkeley",
     "Home of the Computer Systems Research Group (CSRG). Took AT&T UNIX "
     "and developed it into the BSD lineage.",
     ["org", "berkeley", "academic"]),
    ("MIT",
     "AI Lab was the birthplace of Stallman's GNU Project. Earlier, the "
     "site of Multics development that inspired UNIX.",
     ["org", "mit", "academic"]),
    ("Free Software Foundation",
     "Stallman's nonprofit, founded 1985. Stewards the GNU Project and "
     "publishes the GPL family of licenses.",
     ["org", "fsf"]),
    ("Sun Microsystems",
     "Spun out of Stanford by Bechtolsheim and Joy. Shipped SunOS (BSD-"
     "derived) then Solaris (System V). Acquired by Oracle in 2010.",
     ["org", "sun"]),
    ("Unix System Laboratories",
     "AT&T subsidiary that owned and licensed System V UNIX. Sued "
     "Berkeley over BSD; settled in 1994. Sold to Novell.",
     ["org"]),
    ("SCO",
     "Bought UNIX rights from Novell. In the 2000s sued IBM and Linux "
     "users claiming Linux contained SCO-owned code. Lost.",
     ["org"]),

    # --- operating systems ---
    ("Multics",
     "MIT/Bell Labs/GE timesharing project of the 1960s. Too ambitious; "
     "Bell Labs withdrew. Thompson and Ritchie wanted something simpler -- "
     "UNIX was the reaction.",
     ["os"]),
    ("UNIX V1",
     "First released version of UNIX, 1971. Written in PDP-7 assembly. "
     "Single-user, no networking. Already had pipes.",
     ["os"]),
    ("UNIX V6",
     "1975 release; first widely-distributed UNIX outside Bell Labs. "
     "Lions' Commentary on UNIX 6th Edition exposed its source to "
     "generations of students.",
     ["os"]),
    ("UNIX V7",
     "1979 release; the 'last true UNIX' before commercial fragmentation. "
     "Source for both System V (AT&T) and BSD (Berkeley) lineages.",
     ["os"]),
    ("BSD",
     "Berkeley Software Distribution. Started as add-on tapes to AT&T "
     "UNIX, became its own kernel. Lineage of FreeBSD, OpenBSD, NetBSD, "
     "and ultimately Darwin/macOS.",
     ["os", "bsd"]),
    ("4.3BSD",
     "1986 release from Berkeley CSRG. Last BSD to require AT&T license; "
     "the encumbered code was rewritten for 4.4BSD-Lite.",
     ["os", "bsd"]),
    ("System V",
     "AT&T's commercial UNIX line, descended from V7. Established the "
     "STREAMS framework and the SVR4 ABI. Solaris and AIX are SysV "
     "descendants.",
     ["os"]),
    ("MINIX",
     "Tanenbaum's microkernel teaching OS, 1987. Source distributed with "
     "his textbook. Linus Torvalds developed Linux on a MINIX system.",
     ["os"]),
    ("GNU",
     "Stallman's clean-room UNIX-compatible OS project, started 1984. "
     "Userland was nearly complete by 1991 but the Hurd kernel never "
     "shipped; Linux filled the gap.",
     ["os", "fsf"]),
    ("Linux",
     "Torvalds's monolithic kernel, first announced August 1991. "
     "Combined with GNU userland to form the dominant UNIX-like OS.",
     ["os"]),
    ("FreeBSD",
     "Direct descendant of 4.3BSD-Net/2 via 386BSD. Permissive license; "
     "powers Netflix's CDN and the original PlayStation 4 OS.",
     ["os", "bsd"]),
    ("OpenBSD",
     "Forked from NetBSD in 1995 under Theo de Raadt. Famous for proactive "
     "security auditing and the slogan 'only two remote holes in the "
     "default install, in a heck of a long time.'",
     ["os", "bsd"]),
    ("NetBSD",
     "BSD variant emphasizing portability; runs on more architectures than "
     "any other open OS. Origin of pkgsrc.",
     ["os", "bsd"]),
    ("macOS",
     "Apple's desktop OS; Darwin kernel mixes Mach microkernel with FreeBSD "
     "userland. Certified UNIX since 10.5.",
     ["os", "bsd"]),

    # --- languages ---
    ("B language",
     "Ken Thompson's typeless predecessor to C, written for the PDP-7. "
     "Direct descendant of BCPL.",
     ["lang"]),
    ("C language",
     "Ritchie's typed successor to B, 1972. UNIX was rewritten in C "
     "by 1973, making it the first portable OS kernel.",
     ["lang"]),
    ("C++",
     "Stroustrup's object-oriented extension of C, 1985. Compiles via "
     "cfront to C in early versions.",
     ["lang"]),
    ("AWK",
     "Pattern-scanning language by Aho, Weinberger, Kernighan. The 'WK' "
     "is two of the co-authors. Tutorial in K&P's UNIX Programming "
     "Environment.",
     ["lang"]),
    ("Perl",
     "Larry Wall's text-processing language, 1987. Took the AWK / sed / "
     "shell-pipeline workflow and absorbed it into one interpreter.",
     ["lang"]),
    ("Go",
     "Concurrent systems language designed at Google by Pike, Thompson, "
     "and Griesemer. Lineage from Plan 9 / Limbo.",
     ["lang"]),

    # --- concepts / tools ---
    ("pipes",
     "McIlroy's invention; turns the stdout of one process into the stdin "
     "of another. The substrate of the UNIX 'do one thing well' "
     "philosophy.",
     ["concept"]),
    ("everything is a file",
     "Design principle attributed to Thompson and Ritchie: devices, "
     "sockets, processes are all accessed through the file system "
     "interface. Implementation varies; the slogan is stronger than the "
     "code.",
     ["concept"]),
    ("vi",
     "Bill Joy's visual editor, 1976, written at Berkeley for the ADM-3A "
     "terminal. Direct ancestor of Vim.",
     ["tool"]),
    ("Emacs",
     "MIT-grown extensible editor; GNU Emacs by Stallman is the "
     "definitive open implementation. Famous as the other side of the "
     "vi-emacs rivalry.",
     ["tool", "fsf"]),
    ("grep",
     "Pattern-matching command-line tool extracted from ed by Thompson "
     "in 1973. Name comes from the ed command g/re/p.",
     ["tool"]),
    ("The C Programming Language",
     "Kernighan and Ritchie's 1978 book, universally called 'K&R.' "
     "Defined C for a decade and set the template for terse, example-"
     "driven language books.",
     ["book"]),
    ("GPL",
     "GNU General Public License, written by Stallman and Eben Moglen. "
     "Copyleft license requiring derivative works to remain free. "
     "Foundation of Linux's licensing.",
     ["license", "fsf"]),
    ("TCP/IP",
     "Network stack integrated into BSD by Bill Joy and Sam Leffler. "
     "BSD sockets became the de facto cross-platform networking API.",
     ["concept"]),
    ("regex",
     "Thompson's 1968 paper proved regular expressions could be compiled "
     "to NFAs efficiently. Underlies grep, ed, sed, and most text-search "
     "tools.",
     ["concept"]),
]

# Seed edges -- a small set of known relationships so the harness has a
# realistic graph topology (not just isolated nodes). Each entry: (source
# title, target title, kind).
SEED_EDGES: list[tuple[str, str, str]] = [
    ("Ken Thompson",       "Bell Labs",         "works-at"),
    ("Dennis Ritchie",     "Bell Labs",         "works-at"),
    ("Brian Kernighan",    "Bell Labs",         "works-at"),
    ("Doug McIlroy",       "Bell Labs",         "works-at"),
    ("Rob Pike",           "Bell Labs",         "works-at"),
    ("Bjarne Stroustrup",  "Bell Labs",         "works-at"),
    ("Bell Labs",          "AT&T",              "part-of"),
    ("Bill Joy",           "UC Berkeley",       "works-at"),
    ("Bill Joy",           "Sun Microsystems",  "founded"),
    ("Richard Stallman",   "MIT",               "works-at"),
    ("Richard Stallman",   "Free Software Foundation", "founded"),
    ("Ken Thompson",       "UNIX V1",           "created"),
    ("Dennis Ritchie",     "UNIX V1",           "created"),
    ("Dennis Ritchie",     "C language",        "created"),
    ("Ken Thompson",       "B language",        "created"),
    ("Bjarne Stroustrup",  "C++",               "created"),
    ("Larry Wall",         "Perl",              "created"),
    ("Brian Kernighan",    "AWK",               "co-created"),
    ("Ken Thompson",       "grep",              "created"),
    ("Bill Joy",           "vi",                "created"),
    ("Richard Stallman",   "GNU",               "founded"),
    ("Richard Stallman",   "Emacs",             "created"),
    ("Richard Stallman",   "GPL",               "wrote"),
    ("Linus Torvalds",     "Linux",             "created"),
    ("Andrew Tanenbaum",   "MINIX",             "created"),
    ("Theo de Raadt",      "OpenBSD",           "founded"),
    ("Doug McIlroy",       "pipes",             "invented"),
    ("Ken Thompson",       "regex",             "popularized"),
    ("UNIX V1",            "UNIX V6",           "predecessor-of"),
    ("UNIX V6",            "UNIX V7",           "predecessor-of"),
    ("UNIX V7",            "System V",          "predecessor-of"),
    ("UNIX V7",            "BSD",               "predecessor-of"),
    ("BSD",                "4.3BSD",            "predecessor-of"),
    ("4.3BSD",             "FreeBSD",           "predecessor-of"),
    ("4.3BSD",             "NetBSD",            "predecessor-of"),
    ("NetBSD",             "OpenBSD",           "predecessor-of"),
    ("FreeBSD",            "macOS",             "influenced"),
    ("Multics",            "UNIX V1",           "inspired"),
    ("MINIX",              "Linux",             "inspired"),
    ("GNU",                "Linux",             "combined-with"),
    ("B language",         "C language",        "predecessor-of"),
    ("C language",         "C++",               "predecessor-of"),
    ("Brian Kernighan",    "The C Programming Language", "co-wrote"),
    ("Dennis Ritchie",     "The C Programming Language", "co-wrote"),
    ("Unix System Laboratories", "AT&T",        "part-of"),
    ("Unix System Laboratories", "System V",    "owns"),
    ("SCO",                "Linux",             "sued-over"),
    ("UC Berkeley",        "BSD",               "developed"),
    ("Bell Labs",          "UNIX V1",           "developed"),
]


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", default="projects/corpus_unix.db",
                   help="output DB path (will be overwritten if --force)")
    p.add_argument("--force", action="store_true",
                   help="overwrite existing DB")
    p.add_argument("--seed", type=int, default=42)
    args = p.parse_args()

    out = Path(args.out)
    if out.exists():
        if not args.force:
            print(f"refusing to overwrite {out} (pass --force)", file=sys.stderr)
            return 1
        out.unlink()
    out.parent.mkdir(parents=True, exist_ok=True)

    rng = random.Random(args.seed)
    con = sqlite3.connect(out)
    con.executescript(SCHEMA)

    # Lay nodes out in a loose sphere of radius ~25 so they're a realistic
    # zoigraph-scale cluster, not a tight ball or a sparse spread.
    title_to_id: dict[str, int] = {}
    for i, (title, content, tags) in enumerate(NODES):
        # Spherical coords for uniform-ish distribution.
        r = rng.uniform(8, 25)
        theta = rng.uniform(0, 2 * math.pi)
        phi = math.acos(rng.uniform(-1, 1))
        x = r * math.sin(phi) * math.cos(theta)
        y = r * math.sin(phi) * math.sin(theta)
        z = r * math.cos(phi)
        con.execute(
            "INSERT INTO nodes (id, x, y, z, title, content, "
            "first_seen, last_touched, tier) VALUES (?,?,?,?,?,?,?,?,?)",
            (i, round(x, 2), round(y, 2), round(z, 2),
             title, content, 0.0, 0.0, "confirmed"),
        )
        title_to_id[title] = i
        for tag in tags:
            con.execute(
                "INSERT OR IGNORE INTO node_tags (node_id, tag) VALUES (?, ?)",
                (i, tag),
            )

    n_edges_added = 0
    n_edges_skipped = 0
    for src_title, tgt_title, kind in SEED_EDGES:
        src = title_to_id.get(src_title)
        tgt = title_to_id.get(tgt_title)
        if src is None or tgt is None:
            n_edges_skipped += 1
            print(f"  skip edge: {src_title!r} -> {tgt_title!r} "
                  f"(missing node)", file=sys.stderr)
            continue
        con.execute(
            "INSERT INTO edges (source, target, weight, label, kind, "
            "certainty) VALUES (?,?,?,?,?,?)",
            (src, tgt, 1.0, "", kind, "confirmed"),
        )
        n_edges_added += 1

    con.commit()
    con.close()

    print(f"wrote {out}: {len(NODES)} nodes, {n_edges_added} edges "
          f"({n_edges_skipped} skipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
