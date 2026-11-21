#!/usr/bin/env python3
"""
Annotates `mhw_rng_dump.txt` using the MHW 15.23 semantic SQLite database (`mhw.db`),
resolving every `CallSiteRVA` to its enclosing C++ function name, offset, and module.
"""
import os
import re
import sqlite3
import sys

DEFAULT_DB   = os.environ.get("MHW_SEMANTIC_DB", "mhw.db")
DEFAULT_DUMP = os.environ.get("MHW_RNG_DUMP", "mhw_rng_dump.txt")
IMAGE_BASE   = 0x140000000


def main():
    dump_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DUMP
    db_path   = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_DB
    out_path  = sys.argv[3] if len(sys.argv) > 3 else (os.path.splitext(dump_path)[0] + "_annotated.txt")

    if not os.path.exists(dump_path):
        print(f"Error: dump file not found: {dump_path}")
        sys.exit(1)
    if not os.path.exists(db_path):
        print(f"Error: mhw.db not found: {db_path}")
        sys.exit(1)

    db = sqlite3.connect(db_path)
    cur = db.cursor()

    line_re = re.compile(r"^(0x[0-9A-Fa-f]{8})\s+(.*)$")
    out_lines = []
    resolved = 0

    with open(dump_path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            m = line_re.match(line)
            if not m:
                out_lines.append(line)
                continue

            rva = int(m.group(1), 16)
            if rva < 0x80000000:
                va = IMAGE_BASE + rva
                row = cur.execute(
                    "SELECT addr, fullname, module FROM functions WHERE addr <= ? AND end > ? ORDER BY addr DESC LIMIT 1",
                    (va, va),
                ).fetchone()
                if row:
                    fn_addr, fullname, mod = row
                    sym = f"{fullname}+0x{va - fn_addr:X} [{mod}]"
                    resolved += 1
                else:
                    sym = f"0x{va:X} (unindexed region)"
            else:
                sym = "external_dll"

            out_lines.append(f"{line}  |  {sym}")

    with open(out_path, "w", encoding="utf-8") as out:
        out.write("\n".join(out_lines) + "\n")

    print(f"Annotated {resolved} slots -> {out_path}")


if __name__ == "__main__":
    main()
