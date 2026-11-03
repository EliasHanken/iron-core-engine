"""Downloads the ground outside Foundry's walls from Poly Haven (CC0): 1k textures.

    python -I fetch_terrain.py <games/20-foundry/assets/textures>

Each lands in its own folder as diffuse/normal/roughness/ao.jpg, the layout the
game's loadMaterial reads, with a CREDITS.txt naming its authors.
"""
import json
import os
import sys
import urllib.request

TEXTURES = ["aerial_grass_rock", "forrest_ground_01", "coast_sand_01"]
RESOLUTION = "1k"
MAPS = {"Diffuse": "diffuse", "nor_gl": "normal", "Rough": "roughness", "AO": "ao"}


def get(url):
    request = urllib.request.Request(url, headers={"User-Agent": "iron-core-engine asset fetch"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def main():
    out = sys.argv[1]
    for asset in TEXTURES:
        files = json.loads(get(f"https://api.polyhaven.com/files/{asset}"))
        info = json.loads(get(f"https://api.polyhaven.com/info/{asset}"))
        folder = os.path.join(out, asset)
        os.makedirs(folder, exist_ok=True)
        for key, name in MAPS.items():
            entry = files.get(key, {}).get(RESOLUTION, {}).get("jpg")
            if not entry:
                print(f"{asset}: no {key}")
                continue
            with open(os.path.join(folder, name + ".jpg"), "wb") as f:
                f.write(get(entry["url"]))
        authors = ", ".join(info.get("authors", {}).keys())
        with open(os.path.join(folder, "CREDITS.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"{info.get('name', asset)} by {authors}\n"
                    f"Source: https://polyhaven.com/a/{asset}\n"
                    "License: CC0 (https://creativecommons.org/publicdomain/zero/1.0/)\n")
        print(f"{asset:24} by {authors}")


if __name__ == "__main__":
    main()
