#!/usr/bin/env python3
"""Build panplay-catalog.json from PlayStation-Titles (MIT)."""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

SOURCE_URL = "https://raw.githubusercontent.com/andshrew/PlayStation-Titles/main/Json/All_Titles.json"
COVER_RE = re.compile(r"https://cdn\.(?:prosperopatches|orbispatches)\.com/titles/[^\"'\s>]+/icon0\.webp")


def short_title_id(value: str) -> str:
    value = value.strip().upper()
    if len(value) >= 9 and value[:4] in ("CUSA", "PPSA") and value[4:9].isdigit():
        return value[:9]
    return value


def platform_of(title_id: str) -> str:
    if title_id.startswith("PPSA"):
        return "ps5"
    if title_id.startswith("CUSA"):
        return "ps4"
    return ""


def fetch_json(url: str) -> list[dict]:
    req = urllib.request.Request(url, headers={"User-Agent": "panplay-catalog-builder/1.0"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.load(resp)


def fetch_cover(title_id: str) -> str:
    host = "prosperopatches.com" if title_id.startswith("PPSA") else "orbispatches.com"
    url = f"https://{host}/{title_id}"
    req = urllib.request.Request(url, headers={"User-Agent": "panplay-catalog-builder/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            html = resp.read(200_000).decode("utf-8", "replace")
    except urllib.error.HTTPError:
        return ""
    match = COVER_RE.search(html)
    return match.group(0) if match else ""


def compact_entry(row: dict) -> dict:
    title_id = short_title_id(row.get("titleId", ""))
    if not title_id:
        return {}
    entry: dict = {"n": row.get("name") or ""}
    concept_id = row.get("conceptId")
    if concept_id:
        entry["c"] = concept_id
    region = row.get("region")
    if region:
        entry["r"] = region
    publisher = row.get("publisherId")
    if publisher:
        entry["p"] = publisher
    platform = platform_of(title_id)
    if platform:
        entry["x"] = platform
    return entry


def main() -> int:
    parser = argparse.ArgumentParser(description="Build panplay-catalog.json")
    parser.add_argument("--source", default=SOURCE_URL, help="PlayStation-Titles JSON URL")
    parser.add_argument("--output", default="data/panplay-catalog.json", help="Output path")
    parser.add_argument("--fetch-covers", action="store_true", help="Fetch cover URLs from prospero/orbis patches")
    parser.add_argument("--cover-limit", type=int, default=0, help="Max covers to fetch (0 = all when --fetch-covers)")
    parser.add_argument("--platform", choices=("all", "ps4", "ps5"), default="all")
    args = parser.parse_args()

    print(f"Fetching {args.source} ...", file=sys.stderr)
    rows = fetch_json(args.source)
    titles: dict[str, dict] = {}
    for row in rows:
        title_id = short_title_id(row.get("titleId", ""))
        if not title_id:
            continue
        platform = platform_of(title_id)
        if args.platform != "all" and platform != args.platform:
            continue
        entry = compact_entry(row)
        if not entry.get("n"):
            continue
        titles[title_id] = entry

    print(f"Collected {len(titles)} titles", file=sys.stderr)

    if args.fetch_covers:
        candidates = list(titles.keys())
        if args.cover_limit > 0:
            candidates = candidates[: args.cover_limit]
        print(f"Fetching covers for {len(candidates)} titles ...", file=sys.stderr)
        done = 0
        with ThreadPoolExecutor(max_workers=8) as pool:
            futures = {pool.submit(fetch_cover, title_id): title_id for title_id in candidates}
            for future in as_completed(futures):
                title_id = futures[future]
                cover = future.result()
                if cover:
                    titles[title_id]["u"] = cover
                done += 1
                if done % 100 == 0:
                    print(f"  covers {done}/{len(candidates)}", file=sys.stderr)

    payload = {
        "version": 1,
        "source": "PlayStation-Titles (MIT) — https://github.com/andshrew/PlayStation-Titles",
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "generated_from": args.source,
        "count": len(titles),
        "titles": titles,
    }

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh, ensure_ascii=False, separators=(",", ":"))
    print(f"Wrote {out} ({out.stat().st_size // 1024} KiB)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
