"""Build compact move metadata from the public Pokemon Unbound Wiki API."""

from __future__ import annotations

import html
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

import requests
from bs4 import BeautifulSoup


ROOT = Path(__file__).resolve().parents[1]
API = "https://unboundwiki.com/wp-json/wp/v2/wiki"


def normalize(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.casefold())


def load_moves() -> tuple[dict[str, list[int]], dict[int, str]]:
    by_name: dict[str, list[int]] = defaultdict(list)
    names: dict[int, str] = {}
    for line in (ROOT / "data" / "moves.txt").read_text(encoding="utf-8").splitlines():
        if ":" not in line:
            continue
        raw_id, name = line.split(":", 1)
        move_id = int(raw_id)
        names[move_id] = name.strip()
        by_name[normalize(name)].append(move_id)
    return by_name, names


def rows_from_html(rendered: str):
    soup = BeautifulSoup(html.unescape(rendered), "html.parser")
    for row in soup.select("tr"):
        cells = [cell.get_text(" ", strip=True) for cell in row.select("th,td")]
        if len(cells) < 6 or cells[0].lower() in {"lv.", "move"}:
            continue
        offset = 1 if cells[0].isdigit() else 0
        if len(cells) - offset < 6:
            continue
        raw_cells = row.select("th,td")[offset : offset + 6]
        name, move_type, category, power, accuracy, pp = cells[offset : offset + 6]
        image = raw_cells[2].select_one("img")
        if image is not None and image.get("alt"):
            category = image["alt"]
        if not re.fullmatch(r"\d+", pp):
            continue
        yield name, move_type, category, power, accuracy.rstrip("%"), pp


def main() -> None:
    by_name, canonical_names = load_moves()
    observations: dict[int, Counter[tuple[str, ...]]] = defaultdict(Counter)
    dex_moves: dict[int, set[int]] = defaultdict(set)
    dex_abilities: dict[int, set[tuple[int, str]]] = defaultdict(set)
    session = requests.Session()
    for page in range(1, 12):
        response = session.get(API, params={"per_page": 100, "page": page}, timeout=60)
        if response.status_code == 400:
            break
        response.raise_for_status()
        posts = response.json()
        if not posts:
            break
        for post in posts:
            if "/pokemon/" not in post.get("link", ""):
                continue
            rendered = post["content"]["rendered"]
            soup = BeautifulSoup(html.unescape(rendered), "html.parser")
            dex_number = None
            for row in soup.select("tr"):
                cells = [cell.get_text(" ", strip=True) for cell in row.select("th,td")]
                if len(cells) >= 2 and cells[0].strip().lower() == "national dex":
                    match = re.search(r"\d+", cells[1])
                    if match:
                        dex_number = int(match.group())
                    break
            ability_rows = []
            table_rows = soup.select("tr")
            for row_index, row in enumerate(table_rows):
                heading = row.select_one("th")
                if heading is None or heading.get_text(" ", strip=True).lower() != "abilities":
                    continue
                count = int(heading.get("rowspan", "1"))
                for ability_row in table_rows[row_index : row_index + count]:
                    cells = ability_row.select("td")
                    if len(cells) >= 2:
                        ability_rows.append((cells[0].get_text(" ", strip=True), cells[1].get_text(" ", strip=True)))
                break
            if dex_number is not None:
                for index, (ability_name, description) in enumerate(ability_rows):
                    hidden = "hidden ability" in description.casefold() or (len(ability_rows) >= 3 and index == len(ability_rows) - 1)
                    slot = 2 if hidden else min(index, 1)
                    dex_abilities[dex_number].add((slot, ability_name))
            for values in rows_from_html(rendered):
                ids = by_name.get(normalize(values[0]), [])
                if len(ids) == 1:
                    observations[ids[0]][values[1:]] += 1
                    if dex_number is not None:
                        dex_moves[dex_number].add(ids[0])

    output = [
        "# Pokemon Unbound Wiki move metadata, generated from https://unboundwiki.com/",
        "# id|name|type|category|power|accuracy|pp",
    ]
    for move_id in sorted(observations):
        values, _ = observations[move_id].most_common(1)[0]
        output.append("|".join((str(move_id), canonical_names[move_id], *values)))
    (ROOT / "data" / "move_details.txt").write_text("\n".join(output) + "\n", encoding="utf-8")

    dex_map = json.loads((ROOT / "data" / "pokedex_species_map.json").read_text(encoding="utf-8"))["species_to_dex"]
    learnsets = {
        species_id: {"all": sorted(dex_moves[dex_id])}
        for species_id, raw_dex in dex_map.items()
        if (dex_id := int(raw_dex)) in dex_moves
    }
    wiki_output = {
        "source": "https://unboundwiki.com/",
        "note": "Forms sharing a National Dex number use the union documented by the Unbound Wiki.",
        "species_count": len(learnsets),
        "learnsets": learnsets,
    }
    (ROOT / "data" / "wiki_species_learnsets.json").write_text(
        json.dumps(wiki_output, indent=2) + "\n", encoding="utf-8"
    )
    ability_lines = [
        "# Pokemon Unbound Wiki species abilities, generated from https://unboundwiki.com/",
        "# species_id|slot|name",
    ]
    for species_id, raw_dex in sorted(dex_map.items(), key=lambda item: int(item[0])):
        for slot, ability_name in sorted(dex_abilities.get(int(raw_dex), set())):
            ability_lines.append(f"{species_id}|{slot}|{ability_name}")
    (ROOT / "data" / "wiki_species_abilities.txt").write_text("\n".join(ability_lines) + "\n", encoding="utf-8")
    print(json.dumps({"moves": len(observations), "species": len(learnsets), "ability_species": len(dex_abilities)}, indent=2))


if __name__ == "__main__":
    main()
