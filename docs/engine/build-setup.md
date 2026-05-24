# Build setup

Iron Core Engine builds with CMake + MSVC. Most dependencies (GLFW, glad,
stb) come in via `FetchContent` and need no manual setup. Native libraries
that don't play well with `FetchContent` (currently: GameNetworkingSockets)
come in via [vcpkg](https://github.com/microsoft/vcpkg) in manifest mode.

## One-time vcpkg bootstrap

Clone vcpkg anywhere outside this repo:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\Users\elias\Documents\_dev\vcpkg
C:\Users\elias\Documents\_dev\vcpkg\bootstrap-vcpkg.bat
```

(The path is just convention — install vcpkg wherever you like; the CMake
toolchain-file path below points at your chosen location.)

## Configure CMake with the vcpkg toolchain

Pass the toolchain file once at configure time:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/Users/elias/Documents/_dev/vcpkg/scripts/buildsystems/vcpkg.cmake
```

On first configure vcpkg will download and build GameNetworkingSockets and
its transitive deps (libsodium, protobuf, abseil). This takes ~5-10 minutes
on a cold cache. Subsequent configures take seconds — vcpkg caches built
packages in `%LOCALAPPDATA%/vcpkg/`.

After configure, build and test as usual:

```powershell
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

## Updating the dependency baseline

`vcpkg-configuration.json` pins a specific vcpkg registry commit so every
machine resolves to the same package versions. To bump it (e.g. to pick up
a new GNS release), edit the `baseline` field to a newer SHA from
https://github.com/microsoft/vcpkg/commits/master and re-configure.
