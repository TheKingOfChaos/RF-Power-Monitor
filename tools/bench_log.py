"""The log of boards set up at the bench (bench/devices.csv by default).

One row per bench run, newest last; a board run again gets a new row, and
the newest row for a board is the one a reprint uses. The file holds the
default web passwords: it lives in the gitignored bench/ folder.
"""

import csv
import os

FIELDS = ["time", "hostname", "mac", "serial", "firmware", "revision", "update", "password", "label"]


def append(path: str, row: dict) -> None:
    """Add one row (keys from FIELDS) to the CSV at path, with a header row when the file is new."""
    folder = os.path.dirname(path)
    if folder:
        os.makedirs(folder, exist_ok=True)
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, "a", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
        if new:
            writer.writeheader()
        writer.writerow({k: row.get(k, "") for k in FIELDS})


def _mac_digits(text: str) -> str:
    """A MAC as 12 upper-case hex digits, separators removed."""
    return text.replace(":", "").replace("-", "").upper()


def latest(path: str, key: str):
    """The newest row whose hostname (any case), MAC (any separators, any case) or serial equals key, else None."""
    key = key.strip()
    if not key or not os.path.exists(path):
        return None
    found = None
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if (row.get("hostname", "").lower() == key.lower()
                    or (row.get("mac") and _mac_digits(row["mac"]) == _mac_digits(key))
                    or (row.get("serial") and row["serial"].upper() == key.upper())):
                found = row
    return found
