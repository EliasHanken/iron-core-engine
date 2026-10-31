# Forge

Forge builds effects and shaders from plain blocks, with no GLSL. An effect is a small JSON file that compiles to GPU code, so it runs as fast as hand-written shaders.

Open `forge-studio` to edit one live, or use the `forge` tool with no editor at all.

## An effect in 15 lines

```json
{
  "forge": 1,
  "target": "particles",
  "count": 200000,
  "spawn": [{"block": "Point", "spread": 0.2},
            {"block": "Launch", "speed": 7, "spread": 0.8}],
  "move": [{"block": "Gravity"}, {"block": "Drag", "amount": 0.7}],
  "look": [{"block": "ColorOverLife"},
           {"block": "Size", "size": 0.012},
           {"block": "Fade"},
           {"block": "Glow", "intensity": 0.2}]
}
```

- **spawn** runs once when a particle is born. It sets where it starts.
- **move** runs every step, top to bottom.
- **look** decides how it's drawn.

Any value you leave out uses its default, so a block with only a name still works.

## More power

**Layers.** Put several effects in `"layers": [...]`, each with its own `name`, `count` and stacks.

**Shapes.** Name shapes under `"shapes"`, then point blocks at them. `OnShape` starts particles on a surface, `StickToShape` keeps them there, and `Bounce` makes them collide.

```json
"shapes": {"orb": {"type": "LumpySphere", "radius": 2.6}},
"move": [{"block": "StickToShape", "shape": "orb"}]
```

**Ribbons.** Add `"trail": 48` to a layer, and each particle draws as a continuous strip through its last 48 positions.

**Smoke.** Add `"blockLight": true` to a layer, so that it covers what's behind it rather than adding light.

**Nodes.** Any number can be driven by a node instead of a fixed value:

```json
"nodes": {"pulse": {"type": "Sine", "speed": 2, "low": 1, "high": 6}},
"wires": [{"from": "pulse", "to": "layers.0.move.0.strength"}]
```

**Surface shaders.** Set `"target": "surface"` and use a `"surface"` stack of blocks. Later blocks build on earlier ones:

```json
{"forge": 1, "target": "surface",
 "surface": [{"block": "BaseColor", "color": [0.03, 0.03, 0.03]},
             {"block": "LavaCracks"},
             {"block": "Fresnel"}]}
```

Colours are `[r, g, b]` in linear light, or `"#rrggbb"`.

## For AI agents

The `forge` tool gives you the whole loop with no editor:

1. `forge catalog` lists every block, node and shape, with settings, ranges and defaults, as JSON. Read this first, rather than guessing names.
2. Write a `.forge.json` file. Start small, because every block works from its defaults.
3. `forge check <file>` returns JSON. On failure, each issue has `where`, `code`, `message` and usually a `fix`:
   ```json
   {"where": "layers.0.move.0", "code": "unknown-block",
    "message": "There is no block called \"CurlFlw\".", "fix": "Did you mean \"CurlFlow\"?"}
   ```
4. `forge render <file> --at 1,3,5 --out <dir> [--distance 14]` saves stills, so you can look at the result. It opens a window and is capped at 30 fps.
5. `forge compile <file>` prints the generated GLSL, if you need to see it.

If `forge-studio` has the file open, saving it updates the running effect on screen.

### Getting a good look

- Additive particles stack, so brightness scales with `count` and overlap. If the picture blows out to white, lower `Glow` first. Around 700,000 particles usually need a `Glow` near 0.4 with `Strokes`, or about 0.05 without.
- Thin and bright reads better than wide and dim. Try `Size` between 0.005 and 0.02.
- `StretchWithSpeed` turns dots into streaks, and it does more for a look than almost anything else.
- Keep the effect within about 6 metres of the origin, because that's what the default camera frames.

Worked examples are in [examples/](examples/).
