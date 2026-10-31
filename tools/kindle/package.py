"""Packages Kindle as a folder and zip that runs on another Windows PC.

    python package.py <build folder holding kindle.exe> <out folder>

The licensed sounds go into one scrambled audio.pak, never as loose WAVs, and the
Microsoft runtime DLLs are included so that players needn't install them.
"""
import os
import shutil
import struct
import sys
import zipfile

SYSTEM = os.path.join(os.environ["WINDIR"], "System32")
RUNTIME = ["msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll"]

README = """KINDLE
You are a swarm of light in a dead machine. Relight it, and wake the core.

WASD fly   Mouse look   Space rise   Ctrl sink
Shift squeeze   Click ignite   Esc pause   F11 fullscreen

Needs Windows 10 or 11 and a Vulkan 1.3 graphics card.
Made with Iron Core Engine and Forge, by Elias Hanken.
"""


def scramble(data):
    out = bytearray(data)
    for i in range(len(out)):
        out[i] ^= (0x5A + i * 31) & 0xFF
    return bytes(out)


def write_pak(folder, path):
    names = sorted(n for n in os.listdir(folder) if n.endswith(".wav"))
    with open(path, "wb") as f:
        f.write(b"KPAK1" + struct.pack("<I", len(names)))
        for name in names:
            stem = name[:-4].encode()
            with open(os.path.join(folder, name), "rb") as wav:
                data = wav.read()
            f.write(struct.pack("<H", len(stem)) + stem + struct.pack("<I", len(data)) + scramble(data))
    return len(names)


def main():
    build, out = sys.argv[1], sys.argv[2]
    game = os.path.join(out, "Kindle")
    if os.path.exists(game):
        shutil.rmtree(game)
    os.makedirs(game)

    for name in os.listdir(build):
        if name == "kindle.exe" or name.endswith(".dll"):
            shutil.copy(os.path.join(build, name), game)
    for name in RUNTIME:
        shutil.copy(os.path.join(SYSTEM, name), game)

    shutil.copytree(os.path.join(build, "assets"), os.path.join(game, "assets"),
                    ignore=shutil.ignore_patterns("audio-hq"))
    count = write_pak(os.path.join(build, "assets", "audio-hq"), os.path.join(game, "assets", "audio.pak"))
    with open(os.path.join(game, "README.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(README)

    archive = os.path.join(out, "Kindle.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for root, _, files in os.walk(game):
            for name in files:
                path = os.path.join(root, name)
                z.write(path, os.path.relpath(path, out))
    loose = sum(n.endswith(".wav") for _, _, files in os.walk(os.path.join(game, "assets", "audio-hq")) for n in files)
    print(f"{count} sounds packed, {loose} licensed WAVs loose, zip {os.path.getsize(archive) / 1e6:.0f} MB")


if __name__ == "__main__":
    main()
