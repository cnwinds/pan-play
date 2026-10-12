#!/usr/bin/env python3
"""Add cover URLs for a handful of well-known titles (demo / smoke test)."""

import json
import re
import sys
import urllib.request
from pathlib import Path

CATALOG = Path(__file__).resolve().parents[1] / "data" / "panplay-catalog.json"
SAMPLE_IDS = [
    "PPSA01285",
    "PPSA01363",
    "PPSA01287",
    "PPSA01462",
    "PPSA03369",
    "CUSA00341",
    "CUSA02299",
    "CUSA00552",
]
COVER_RE = re.compile(
    r"https://cdn\.(?:prosperopatches|orbispatches)\.com/titles/[^\"'\s>]+/icon0\.webp"
)


def fetch_cover(title_id: str) -> str:
    host = "prosperopatches.com" if title_id.startswith("PPSA") else "orbispatches.com"
    req = urllib.request.Request(
        f"https://{host}/{title_id}",
        headers={"User-Agent": "panplay-catalog-builder/1.0"},
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        html = resp.read(200_000).decode("utf-8", "replace")
    match = COVER_RE.search(html)
    return match.group(0) if match else ""


def main() -> int:
    data = json.loads(CATALOG.read_text(encoding="utf-8"))
    titles = data.get("titles", {})
    for title_id in SAMPLE_IDS:
        if title_id not in titles:
            print(f"missing {title_id}", file=sys.stderr)
            continue
        cover = fetch_cover(title_id)
        if cover:
            titles[title_id]["u"] = cover
            print(f"ok {title_id}", file=sys.stderr)
        else:
            print(f"no cover {title_id}", file=sys.stderr)
    CATALOG.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
