"""Downloads Foundry's props from Poly Haven (CC0), as 1k glTF with their textures.

    python -I fetch_props.py <games/20-foundry/assets/models>

Each model lands in its own folder with a CREDITS.txt naming its authors. Poly Haven's
licence is CC0, so these are committed with the game.
"""
import json
import os
import sys
import urllib.request

PROPS = [
    "Barrel_01", "Barrel_02", "barrel_03", "wooden_crate_02", "old_military_crate",
    "propane_tank", "small_lpg_tank", "steel_frame_shelves_01", "hanging_industrial_lamp",
    "concrete_road_barrier_02", "modular_industrial_pipes_01", "old_military_compressor",
    "metal_jerrycan", "cement_bag", "old_tyre", "portable_generator",
    # The level art pass: the hall's working kit, and the yard's street furniture.
    "overhead_crane", "portable_welding_cart", "tool_cart", "metal_tool_chest", "worn_metal_rack",
    "old_drill_press", "hand_truck", "ladder_sectioned_01", "modular_airduct_circular_01", "power_box_01",
    "korean_fire_extinguisher_01", "cardboard_box_01", "plastic_crate_03", "WetFloorSign_01",
    "security_light", "exterior_aircon_unit", "utility_box_01", "utility_box_02",
    "metal_trash_can", "rusted_wheel_rim_01", "water_manhole_cover", "covered_car", "security_camera_01",
]
RESOLUTION = "1k"


def get(url):
    request = urllib.request.Request(url, headers={"User-Agent": "iron-core-engine asset fetch"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def main():
    out = sys.argv[1]
    for asset in PROPS:
        files = json.loads(get(f"https://api.polyhaven.com/files/{asset}"))
        info = json.loads(get(f"https://api.polyhaven.com/info/{asset}"))
        gltf = files["gltf"][RESOLUTION]["gltf"]
        folder = os.path.join(out, asset)
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, asset + ".gltf"), "wb") as f:
            f.write(get(gltf["url"]))
        for relative, entry in gltf.get("include", {}).items():
            target = os.path.normpath(os.path.join(folder, relative))
            if not target.startswith(os.path.normpath(folder)):
                raise SystemExit(f"refusing a path outside the folder: {relative}")
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(get(entry["url"]))
        authors = ", ".join(info.get("authors", {}).keys())
        with open(os.path.join(folder, "CREDITS.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"{info.get('name', asset)} by {authors}\n"
                    f"Source: https://polyhaven.com/a/{asset}\n"
                    "License: CC0 (https://creativecommons.org/publicdomain/zero/1.0/)\n")
        print(f"{asset:28} {info.get('polycount', '?'):>7} tris  by {authors}")


if __name__ == "__main__":
    main()
