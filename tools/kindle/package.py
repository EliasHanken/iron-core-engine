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

# Every library that ends up in the game, by its folder under vcpkg's share directory.
LIBRARIES = ["abseil", "fmt", "gamenetworkingsockets", "glslang", "imgui", "joltphysics", "nlohmann-json",
             "openal-soft", "openssl", "protobuf", "stb", "tinygltf", "utf8-range", "vulkan-headers",
             "vulkan-loader", "vulkan-memory-allocator"]

README = """KINDLE
You are a swarm of light in a dead machine. Relight it, and wake the core.

WASD fly   Mouse look   Space rise   Ctrl sink
Shift squeeze   Click ignite   Esc pause   F11 fullscreen

Needs Windows 10 or 11 and a Vulkan 1.3 graphics card.
Made with Iron Core Engine and Forge, by Elias Hanken.

Licences for the software in this game are in the licenses folder. The sounds in
audio.pak are licensed for this game only, and may not be extracted or passed on.
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


def copy_licenses(build, game):
    """The licences of everything shipped, which many of them require alongside a binary."""
    root = os.path.abspath(os.path.join(build, "..", "..", ".."))
    repo = os.path.abspath(os.path.join(root, ".."))
    folder = os.path.join(game, "licenses")
    os.makedirs(folder)
    share = os.path.join(root, "vcpkg_installed", "x64-windows", "share")
    for name in LIBRARIES:
        shutil.copy(os.path.join(share, name, "copyright"), os.path.join(folder, name + ".txt"))
    shutil.copy(os.path.join(root, "_deps", "glfw-src", "LICENSE.md"), os.path.join(folder, "glfw.txt"))
    shutil.copy(os.path.join(repo, "third_party", "imgui-node-editor", "LICENSE"),
                os.path.join(folder, "imgui-node-editor.txt"))
    for name in ("LICENSE", "NOTICE", "THIRD_PARTY.md"):
        shutil.copy(os.path.join(repo, name), os.path.join(folder, "iron-core-" + name))


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
    copy_licenses(build, game)
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
