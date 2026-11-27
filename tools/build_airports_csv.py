#!/usr/bin/env python3
"""Build data/airports.csv from the OurAirports airport dump.

Usage:
    tools/build_airports_csv.py /path/to/airports.csv

The dump is published in the public domain:
    https://davidmegginson.github.io/ourairports-data/airports.csv

Only airports with a four-letter ICAO identifier and either scheduled service
or a large/medium type are kept: that is what ACARS flight plans can name.
Names are reduced to ASCII (the 320x170 UI font has no accents) and the
municipality is stripped of parenthetical and regional suffixes, so a lookup
yields a short display label such as "TAIYUAN" or "GUANGZHOU".
"""

import csv
import sys
import unicodedata
from pathlib import Path

COLUMNS = ["ident", "name", "city", "country", "iata"]

# Characters that NFKD does not decompose into an ASCII base letter.
SPECIAL = {
    "ð": "d", "þ": "th", "ø": "o", "ł": "l", "ß": "ss", "æ": "ae",
    "œ": "oe", "đ": "d", "ı": "i", "ŋ": "n", "ħ": "h", "ŧ": "t",
}

NAME_SUFFIXES = [
    " International Airport",
    " Regional Airport",
    " Municipal Airport",
    " Airport",
    " Air Base",
    " Airbase",
    " Airfield",
    " Heliport",
    " Airport",
]


def ascii_text(text: str) -> str:
    out = []
    for char in unicodedata.normalize("NFKD", text):
        if ord(char) < 128:
            out.append(char)
        elif char.lower() in SPECIAL:
            out.append(SPECIAL[char.lower()])
    return "".join(out)


def clean_city(text: str) -> str:
    text = text.split("(")[0].split(",")[0]
    return " ".join(ascii_text(text).split())


def clean_name(text: str) -> str:
    text = " ".join(ascii_text(text).split())
    for suffix in NAME_SUFFIXES:
        if text.endswith(suffix):
            return text[: -len(suffix)]
    return text


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    source = Path(sys.argv[1])
    output = Path(__file__).resolve().parent.parent / "data" / "airports.csv"
    output.parent.mkdir(parents=True, exist_ok=True)

    kept = 0
    with source.open(newline="", encoding="utf-8") as handle, output.open(
        "w", newline="", encoding="utf-8"
    ) as out:
        reader = csv.DictReader(handle)
        writer = csv.writer(out, lineterminator="\n")
        writer.writerow(COLUMNS)
        for row in reader:
            ident = row["ident"].strip().upper()
            if len(ident) != 4 or not ident.isalnum() or not ident[0].isalpha():
                continue
            scheduled = row["scheduled_service"] == "yes"
            large = row["type"] in ("large_airport", "medium_airport")
            if not (scheduled or large):
                continue
            city = clean_city(row["municipality"])
            name = clean_name(row["name"])
            if not city and not name:
                continue
            writer.writerow(
                [ident, name, city, row["iso_country"], row["iata_code"]]
            )
            kept += 1

    print(f"wrote {output} ({kept} airports, {output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
