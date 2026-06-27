#!/usr/bin/env python3
"""Reassemble a regenerated analyzer `.test` golden from a Bazel test.log.

GoogleSQL analyzer tests use file_based_test_driver, which (when run with
--file_based_test_driver_generate_test_output, the default) writes the *new*
expected golden into the test log, split across one or more blocks of the form:

    ****TEST_OUTPUT_BEGIN**** NEW_TEST_RUN <path>
    <content...>
    ****TEST_OUTPUT_END****
    ****TEST_OUTPUT_BEGIN**** <path>          # continuation block(s), no NEW_TEST_RUN
    <more content...>
    ****TEST_OUTPUT_END****

Long single lines are split with a `***MERGE_TOO_LONG_LINE***` sentinel that must
be rejoined. There is no `extract_test_output.py` in the open-source export, so
this script stands in for it for the single-file case.

Usage:
    extract_golden.py <test.log> <dest.test> [--prefix TEST_OUTPUT] [--dry-run]

Matches blocks whose logged path ends with the basename of <dest.test>, then
overwrites <dest.test> with the reassembled content (or prints a summary with
--dry-run). Verify the result with `git diff <dest.test>` afterwards.
"""
import argparse
import os
import sys


def reassemble(log_path, prefix):
    begin = f"****{prefix}_BEGIN****"
    end = f"****{prefix}_END****"
    # path (logged runfiles path) -> list of body lines, in encounter order
    by_path = {}
    order = []
    with open(log_path, errors="replace") as f:
        lines = f.read().split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        idx = line.find(begin)
        if idx == -1:
            i += 1
            continue
        header = line[idx + len(begin):].strip()
        is_new = header.startswith("NEW_TEST_RUN ")
        path = header[len("NEW_TEST_RUN "):].strip() if is_new else header
        # collect body until the END marker line
        body = []
        i += 1
        while i < len(lines) and end not in lines[i]:
            body.append(lines[i])
            i += 1
        # i now points at the END line (or EOF)
        if is_new or path not in by_path:
            by_path[path] = []
            if path not in order:
                order.append(path)
        by_path[path].extend(body)
        i += 1
    return by_path, order


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("dest")
    ap.add_argument("--prefix", default="TEST_OUTPUT")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    by_path, order = reassemble(args.log, args.prefix)
    if not by_path:
        sys.exit(f"No ****{args.prefix}_BEGIN**** blocks found in {args.log}. "
                 f"Did the test run with output generation on (the default)?")

    want = os.path.basename(args.dest)
    matches = [p for p in order if os.path.basename(p) == want]
    if not matches:
        found = "\n  ".join(os.path.basename(p) for p in order)
        sys.exit(f"No blocks for '{want}'. Goldens present in log:\n  {found}")
    if len(matches) > 1:
        sys.exit(f"Ambiguous: multiple logged paths basename-match '{want}': {matches}")

    body_lines = by_path[matches[0]]
    text = "\n".join(body_lines)
    if text and not text.endswith("\n"):
        text += "\n"
    # rejoin lines that the driver split because they exceeded the log buffer
    text = text.replace("\n***MERGE_TOO_LONG_LINE***\n", "")

    if args.dry_run:
        print(f"Would write {len(text.splitlines())} lines to {args.dest} "
              f"(from logged path {matches[0]}).")
        return
    with open(args.dest, "w") as f:
        f.write(text)
    print(f"Wrote {args.dest}: {len(text.splitlines())} lines.")


if __name__ == "__main__":
    main()
