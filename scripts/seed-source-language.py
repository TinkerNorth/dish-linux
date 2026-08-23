#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Fills the SOURCE-language catalogue from its own source strings.
#
# dish_en.ts is the odd one out: the app is authored in English, so `qsTr()`
# already returns the right words and Qt falls back to <source> for anything
# unfinished. That fallback is invisible to a coverage count, which would leave
# the source catalogue permanently "untranslated" and the gate permanently off
# for the one language nobody has to translate.
#
# Seeding translation := source makes the count mean the same thing in every
# catalogue, so the gate is one uniform rule rather than five languages plus an
# exception.
#
# Plural messages are deliberately LEFT unfinished for a human: a source string
# carries one form, and English needs a distinct singular. Filling those from
# <source> would silently ship "1 slots free". They are the only entries in this
# file a person still has to write, which is the whole point of running lupdate
# with -pluralonly elsewhere in the ecosystem.
#
#   scripts/seed-source-language.py translations/dish_en.ts
#   scripts/seed-source-language.py --check translations/dish_en.ts

import argparse
import re
import sys

# One <message>…</message> block. Non-greedy, DOTALL: blocks never nest.
MESSAGE = re.compile(r"<message\b[^>]*>.*?</message>", re.DOTALL)
SOURCE = re.compile(r"<source>(.*?)</source>", re.DOTALL)
UNFINISHED_EMPTY = '<translation type="unfinished"></translation>'


def seed_block(block: str) -> str:
    # numerus blocks carry <numerusform> slots and need a real decision.
    if 'numerus="yes"' in block:
        return block
    if UNFINISHED_EMPTY not in block:
        return block
    match = SOURCE.search(block)
    if match is None:
        return block
    # The source text is already XML-escaped in the file, so it drops straight
    # into the translation element with no re-escaping.
    return block.replace(UNFINISHED_EMPTY, f"<translation>{match.group(1)}</translation>")


def seed(text: str) -> str:
    return MESSAGE.sub(lambda m: seed_block(m.group(0)), text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalogue", help="path to the source-language .ts")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if seeding would change the file; write nothing")
    args = parser.parse_args()

    with open(args.catalogue, encoding="utf-8") as handle:
        before = handle.read()
    after = seed(before)

    if before == after:
        return 0
    if args.check:
        print(f"{args.catalogue} has unseeded source-language entries; "
              f"run scripts/seed-source-language.py {args.catalogue}", file=sys.stderr)
        return 1
    with open(args.catalogue, "w", encoding="utf-8") as handle:
        handle.write(after)
    filled = before.count(UNFINISHED_EMPTY) - after.count(UNFINISHED_EMPTY)
    print(f"{args.catalogue}: seeded {filled} entries from their source strings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
