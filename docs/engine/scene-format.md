# Scene file format

The scene file format is the authored-level format for the editor track.
A `.json` scene file describes entities (meshes + materials + transforms),
lighting, fog, and clear color — pure data, no GPU handles. The C++ type
is `iron::SceneFile`. It is the data foundation the editor UI will read
and write in later milestones; the sandbox runtime demonstrates loading and
rendering one end-to-end today.

## C++ API

```cpp
// engine/scene/SceneIO.h
std::optional<SceneFile> loadSceneFile(const std::string& path);
bool                     saveSceneFile(const SceneFile&, const std::string& path);
```

`loadSceneFile` returns `nullopt` on a missing file or malformed JSON;
omitted fields in a well-formed file use their defaults (described per-field
below). `saveSceneFile` writes pretty-printed JSON with 2-space indentation
and returns `false` if the file cannot be opened.

`SceneFile` is pure data (no GPU handles), so the load/save round-trip is
unit-tested headlessly. `tests/test_scene_io.cpp` covers four cases:
round-trip preserving all fields; malformed JSON → `nullopt`; missing
file → `nullopt`; minimal scene uses defaults.

Backed by [nlohmann/json](https://github.com/nlohmann/json) vendored under
`third_party/json/nlohmann/json.hpp`.

## Top-level keys

| Key           | Type                     | Default             |
| ------------- | ------------------------ | ------------------- |
| `clearColor`  | `[r, g, b]`              | `[0.5, 0.6, 0.7]`  |
| `sun`         | object — see below       | see below           |
| `fog`         | object — see below       | see below           |
| `pointLights` | array of objects         | `[]`                |
| `entities`    | array of objects         | `[]`                |

### `clearColor`

Sky/background color as an RGB triple, values in `[0, 1]`.

```json
"clearColor": [0.45, 0.55, 0.7]
```

### `sun`

A single directional light. Scene ambient lives here as `ambient`; there
is no separate scene-level ambient key.

| Field       | Type        | Default       |
| ----------- | ----------- | ------------- |
| `direction` | `[x, y, z]` | `[0, -1, 0]`  |
| `color`     | `[r, g, b]` | `[1, 1, 1]`   |
| `ambient`   | float       | `0.1`         |

```json
"sun": { "direction": [-0.5, -1.0, -0.3], "color": [1.0, 0.97, 0.9], "ambient": 0.2 }
```

### `fog`

Exponential fog. Set `density` to `0.0` (the default) to disable fog
entirely.

| Field     | Type        | Default       |
| --------- | ----------- | ------------- |
| `color`   | `[r, g, b]` | `[0.7, 0.6, 0.5]` |
| `density` | float       | `0.0`         |

```json
"fog": { "color": [0.45, 0.55, 0.7], "density": 0.0 }
```

### `pointLights`

Array of omnidirectional point lights.

| Field       | Type        | Default    |
| ----------- | ----------- | ---------- |
| `position`  | `[x, y, z]` | `[0,0,0]`  |
| `color`     | `[r, g, b]` | `[1,1,1]`  |
| `intensity` | float       | `1.0`      |
| `range`     | float       | `5.0`      |

```json
"pointLights": [
  { "position": [-2.0, 3.0, 2.0], "color": [1.0, 0.6, 0.3], "intensity": 2.5, "range": 14.0 }
]
```

## `SceneEntity`

Each element of the `entities` array is a `SceneEntity`.

| Field      | Type        | Default          |
| ---------- | ----------- | ---------------- |
| `name`     | string      | `""`             |
| `position` | `[x, y, z]` | `[0, 0, 0]`      |
| `rotation` | `[x, y, z, w]` — quaternion | `[0, 0, 0, 1]` |
| `scale`    | `[x, y, z]` | `[1, 1, 1]`      |
| `mesh`     | object      | see below        |
| `material` | object      | see below        |

`rotation` is stored as a quaternion in `[x, y, z, w]` order (i.e.
the real part `w` is last). The identity quaternion is `[0, 0, 0, 1]`.

### `MeshRef`

The `mesh` field selects what geometry to draw. Two variants are
supported: a built-in primitive or a path to a glTF file. If both are
present, `primitive` wins.

| Field       | Type   | Values                      |
| ----------- | ------ | --------------------------- |
| `primitive` | string | `"cube"` or `"plane"`       |
| `gltfPath`  | string | path to a `.gltf` / `.glb`  |

Paths resolve relative to the executable's directory (see
[Path resolution](#path-resolution) below).

An unknown `primitive` value, or a `gltfPath` that fails to load,
causes that entity to be skipped with a logged warning. The rest of the
scene still renders.

```json
"mesh": { "primitive": "cube" }
"mesh": { "gltfPath": "assets/damaged-helmet/DamagedHelmet.gltf" }
```

### `MaterialDef`

The `material` field controls textures, emissive color, UV tiling, and
reflectivity. All fields are optional; an empty string or an omitted path
falls back to the engine default for that slot (white texture / flat-normal /
no specular).

| Field         | Type        | Default       | Notes                          |
| ------------- | ----------- | ------------- | ------------------------------ |
| `albedoPath`  | string      | `""`          | `""` → white texture           |
| `normalPath`  | string      | `""`          | `""` → flat-normal texture     |
| `specularPath`| string      | `""`          | `""` → no-specular texture     |
| `emissive`    | `[r, g, b]` | `[0, 0, 0]`   |                                |
| `uvScale`     | float       | `1.0`         |                                |
| `reflectivity`| float       | `0.0`         |                                |

For glTF entities, the glTF's own embedded material textures are used
first; scene-file texture paths only fill slots the glTF did not provide.
So a `material` block on a helmet entity that already carries albedo/normal
inside the `.gltf` will only take effect for paths the glTF left empty.

```json
"material": { "emissive": [0.6, 0.15, 0.1] }
"material": { "reflectivity": 0.2 }
```

## Worked example — `demo.json`

The `games/11-sandbox` demo ships a scene file that exercises all four
entity types (floor plane, two emissive cubes, a static glTF):

```json
{
  "clearColor": [0.45, 0.55, 0.7],
  "sun": { "direction": [-0.5, -1.0, -0.3], "color": [1.0, 0.97, 0.9], "ambient": 0.2 },
  "fog": { "color": [0.45, 0.55, 0.7], "density": 0.0 },
  "pointLights": [
    { "position": [-2.0, 3.0, 2.0], "color": [1.0, 0.6, 0.3], "intensity": 2.5, "range": 14.0 }
  ],
  "entities": [
    {
      "name": "floor",
      "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [20, 1, 20],
      "mesh": { "primitive": "plane" },
      "material": { "emissive": [0.05, 0.05, 0.06], "uvScale": 1.0 }
    },
    {
      "name": "cube-red",
      "position": [-2, 1, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1],
      "mesh": { "primitive": "cube" },
      "material": { "emissive": [0.6, 0.15, 0.1] }
    },
    {
      "name": "cube-green",
      "position": [0, 1, -2], "rotation": [0, 0, 0, 1], "scale": [1, 2, 1],
      "mesh": { "primitive": "cube" },
      "material": { "emissive": [0.1, 0.5, 0.15] }
    },
    {
      "name": "helmet",
      "position": [2.5, 1.5, 0], "rotation": [0, 0.3826834, 0, 0.9238795], "scale": [1.5, 1.5, 1.5],
      "mesh": { "gltfPath": "assets/damaged-helmet/DamagedHelmet.gltf" },
      "material": { "reflectivity": 0.2 }
    }
  ]
}
```

The sandbox loads this file at startup, spawns a free-fly camera, and
renders the scene under the directional + point lighting. The clear color
and fog color are intentionally set to the same value so fog blends into
the sky.

Run the sandbox with:

```powershell
.\build-vk\games\11-sandbox\Debug\sandbox.exe
```

## Path resolution

All asset paths in a scene file — the scene file itself, `gltfPath`
entries, and texture paths — resolve relative to the runtime's executable
directory. The build system copies `assets/` there via a CMake `POST_BUILD`
step, so the paths are stable regardless of which directory the executable
is launched from.

## Known v1 limitations

- **Static meshes only.** Skinned/animated entities are not yet
  representable in the scene format. Skeletal mesh support in the scene
  layer is a future track.
- **No collision shapes, audio emitters, or skybox.** These will arrive
  as the editor gains UI to author them.
- **`cube` and `plane` only.** No sphere or other primitive — extend
  `PrimitiveKind` and the corresponding loader when needed.
- **No live scene hot-reload.** Edit `demo.json`, save, then relaunch
  the sandbox to see the change. File-watching for scene files is
  deferred alongside the editor track.
