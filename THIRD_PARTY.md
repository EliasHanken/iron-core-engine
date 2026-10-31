# Third-party material

[`LICENSE`](LICENSE) (Apache-2.0) covers Iron Core's own code and original work. The material below belongs to its authors and stays under its own terms.

## Libraries

Fetched at build time through vcpkg or CMake, unless the table says otherwise.

| Library | Used for | Licence |
|---|---|---|
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | Rigid bodies and character physics | MIT |
| [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) | Networking | BSD-3-Clause |
| [OpenAL Soft](https://github.com/kcat/openal-soft) | Audio, linked as a replaceable DLL | LGPL-2.1-or-later |
| [Dear ImGui](https://github.com/ocornut/imgui) | Editor UI | MIT |
| [imgui-node-editor](https://github.com/thedmd/imgui-node-editor) | Node graph, vendored in `third_party/` | MIT |
| [GLFW](https://github.com/glfw/glfw) | Windows and input | Zlib |
| [glad](https://github.com/Dav1dde/glad) | OpenGL loader | MIT |
| [Vulkan Headers and Loader](https://github.com/KhronosGroup) | Vulkan | Apache-2.0 |
| [glslang](https://github.com/KhronosGroup/glslang) | Runtime shader compiling | BSD-3-Clause and others |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | GPU memory | MIT |
| [tinygltf](https://github.com/syoyo/tinygltf) | glTF loading | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON, vendored in `third_party/` | MIT |
| [stb](https://github.com/nothings/stb) | Images and fonts, vendored in `third_party/` | MIT or public domain |
| [dr_libs](https://github.com/mackron/dr_libs) | WAV decoding, vendored in `third_party/` | MIT-0 or public domain |
| [Fork Awesome](https://forkaweso.me) | Editor icons, `third_party/IconsForkAwesome.h` | SIL OFL 1.1 (font), MIT (code) |

GameNetworkingSockets also brings in OpenSSL (Apache-2.0), Protobuf (BSD-3-Clause) and Abseil (Apache-2.0).

## Fonts

| Font | Where | Licence |
|---|---|---|
| Michroma | `games/17-iron-core/assets/fonts/` | SIL OFL 1.1, text beside the font |
| Roboto | `games/*/assets/fonts/` | Apache-2.0 |
| Fork Awesome | `games/11-sandbox/assets/fonts/` | SIL OFL 1.1, text beside the font |

## Models

From the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets).

| Model | Where | Credit | Licence |
|---|---|---|---|
| **Damaged Helmet** | `games/10-gltf-viewer/assets/damaged-helmet/`, `games/11-sandbox/assets/damaged-helmet/` | Earlier model by theblueturtle_, rebuilt by ctxwing | **CC BY-NC 4.0 and CC BY 4.0: non-commercial use only** |
| Fox | `games/07-net-shooter/assets/fox/`, `tests/assets/gltf/` | Model: public domain. Rigging by tomkranis. Conversion by AsoboStudio and scurest | CC0 and CC BY 4.0 |
| Rigged Simple | `games/10-gltf-viewer/assets/rigged-simple/`, `tests/assets/gltf/` | Cesium | CC BY 4.0 |
| Box, Box Textured | `tests/assets/gltf/` | Cesium | CC BY 4.0. The Cesium logo is a trademark of Cesium |
| Triangle | `tests/assets/gltf/` | Khronos | CC0 |

The Damaged Helmet is only a viewer and editor sample. Anyone building a commercial product from this repo should remove it.

## Textures and environments

| Asset | Where | Credit | Licence |
|---|---|---|---|
| Brick, ground, metal and wood | `assets/cc0/` | [Poly Haven](https://polyhaven.com), see each `CREDITS.txt` | CC0 |
| Garden HDRI | `games/11-sandbox/assets/hdri/` | [Poly Haven](https://polyhaven.com), see `CREDITS.txt` | CC0 |
| Rope | `games/02-strandbound/assets/` | ambientCG Rope002 | CC0 |

## Sound

| Asset | Where | Credit | Licence |
|---|---|---|---|
| Kindle's free sound set | `games/19-kindle/assets/audio/` | Listed in `CREDITS.md` there | CC0 |
| Rocket explosion | `games/07-net-shooter/assets/sfx/` | tommccann, [freesound.org 235968](https://freesound.org/s/235968/) | CC0 |
| Rifle shot | `games/07-net-shooter/assets/sfx/` | kyles, [freesound.org 450852](https://freesound.org/s/450852/) | CC0 |
| Rocket launch | `games/07-net-shooter/assets/sfx/` | Jarusca, [freesound.org 521377](https://freesound.org/s/521377/) | CC0 |
| Footsteps | `games/07-net-shooter/assets/sfx/` | Fission9, [freesound.org 521587](https://freesound.org/s/521587/) | CC0 |
| Hit | `games/07-net-shooter/assets/sfx/` | insanity54, [freesound.org 276600](https://freesound.org/s/276600/) | CC0 |
| Jump | `games/07-net-shooter/assets/sfx/` | cartchaos, [freesound.org 678839](https://freesound.org/s/678839/) | CC0 |
| Death | `games/07-net-shooter/assets/sfx/` | measly_whelp, [freesound.org 710650](https://freesound.org/s/710650/) | CC0 |

## Source not recorded

These files have no recorded author, so they aren't covered by `LICENSE` and no permission is offered for them:

- `games/01-spinning-cube/assets/crate.jpg`, `games/02-strandbound/assets/crate.jpg`
- `games/11-sandbox/assets/node-header-bg.png`
- `tests/assets/sfx/test-beep.wav`

## Not in this repository

The downloadable build of Kindle also contains licensed sound recordings and a score, packed in `audio.pak`. They aren't part of this repository, aren't covered by `LICENSE`, and may not be extracted or redistributed as sound effects.
