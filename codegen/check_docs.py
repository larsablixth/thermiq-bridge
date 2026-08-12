#!/usr/bin/env python3
"""Check the docs against the code they describe.

Two directions, and the second is the one that matters:

- Every environment variable the program reads should be documented, or nobody
  can find it.
- Every environment variable the docs mention must actually be read by the
  program. AI_INSTALL.md is followed literally by an agent that cannot tell a
  real setting from a plausible-looking one, so an invented variable there
  becomes a bridge configured with a setting that does nothing.

Usage:
    python3 codegen/check_docs.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SOURCES = ["src/config.c", "src/discover.c", "src/main.c"]
DOCS = ["README.md", "AI_INSTALL.md"]

VARIABLE = re.compile(r"THERMIQ_[A-Z_]+")

# Read by the program but deliberately undocumented: an escape hatch the tests
# use, not a setting anyone should reach for.
INTERNAL = {"THERMIQ_OPTIONS_FILE"}

# Real settings that are nonetheless not add-on options: the Supervisor owns
# the listening address through ingress, and the discovery timeout belongs to
# a command-line mode an add-on never runs.
NOT_ADDON_OPTIONS = {
    "THERMIQ_HTTP_HOST",
    "THERMIQ_HTTP_PORT",
    "THERMIQ_DISCOVER_SECONDS",
}


def variables_in(paths: list[str]) -> dict[str, set[str]]:
    found: dict[str, set[str]] = {}
    for path in paths:
        text = (REPO / path).read_text(encoding="utf-8")
        for name in VARIABLE.findall(text):
            found.setdefault(name, set()).add(path)
    return found


def main() -> int:
    in_code = variables_in(SOURCES)
    in_docs = variables_in(DOCS)
    problems = []

    undocumented = set(in_code) - set(in_docs) - INTERNAL
    for name in sorted(undocumented):
        problems.append(
            f"{name} is read by {', '.join(sorted(in_code[name]))} but appears in "
            f"none of {', '.join(DOCS)}"
        )

    invented = set(in_docs) - set(in_code)
    for name in sorted(invented):
        problems.append(
            f"{name} is documented in {', '.join(sorted(in_docs[name]))} but no "
            f"source file reads it - either it was renamed or it never existed"
        )

    # The add-on maps its options onto these names, so it can drift too.
    addon = (REPO / "addon/config.yaml").read_text(encoding="utf-8")
    schema = addon.split("schema:", 1)[1] if "schema:" in addon else ""
    options = set(re.findall(r"^  ([a-z_]+):", schema, re.MULTILINE))
    expected = {
        name[len("THERMIQ_") :].lower()
        for name in in_code
        if name not in INTERNAL and name not in NOT_ADDON_OPTIONS
    }
    for missing in sorted(expected - options):
        problems.append(
            f"addon/config.yaml has no option for THERMIQ_{missing.upper()}; "
            f"add-on users cannot set it"
        )
    for extra in sorted(options - expected):
        problems.append(
            f"addon/config.yaml offers an option '{extra}' that maps to "
            f"THERMIQ_{extra.upper()}, which nothing reads"
        )

    if problems:
        print("Documentation does not match the code:\n", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\nUpdate the docs, or the add-on schema, to match src/.",
            file=sys.stderr,
        )
        return 1

    documented = len(set(in_code) - INTERNAL)
    print(f"{documented} environment variables, all documented and all real")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
