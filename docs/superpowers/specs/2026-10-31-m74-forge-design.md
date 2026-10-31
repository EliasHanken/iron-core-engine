# M74: Forge, a visual layer for effects and shaders

## Why

Iron Core proved the engine can render AAA effects, but every effect is hand-written GLSL. Forge is a layer on top of the engine for building effects and shaders without writing GLSL, live, in the editor.

The user wants three things from it:

1. **Power:** it can build very complex shaders and effects.
2. **Simple:** easier than Unity's Visual Effect Graph, not a copy of it.
3. **AI-native:** an AI can read, write, check and render it as easily as a person can.

It also becomes the next portfolio piece: effect on the left, graph on the right, and something striking built from nothing in about 60 seconds.

## What makes Unity's hard, and what Forge does instead

| Unity VFX Graph | Forge |
|---|---|
| Learn Contexts, Blocks, attributes, spaces and capacity before anything appears | Three plain stacks: **Spawn**, **Move**, **Look** |
| An empty graph shows nothing | Every effect starts from something working, and never goes blank |
| Many small maths nodes to express one idea | One block per idea, working with its defaults: Curl Flow, Stick To Shape, Glow |
| Compile errors in shader terms | Plain errors on the block at fault, and the last good version keeps running |
| Built for a mouse in an editor | The file, the checker and the renderer all work with no editor at all |

Deep graphs are still there. Any block value can be driven by a wire from a full node graph, so simple effects stay simple and hard ones stay possible.

## Goals

1. **One layer, several targets.** The same graph, nodes and tools build particle effects first, then surface shaders. Post effects and fluids can follow.
2. **Rebuild the orb in Forge,** matching the hand-written look and within 10% of its GPU cost.
3. **Live editing.** A change appears on screen in under a second, without restarting.
4. **An AI can do the whole loop headless:** discover what exists, write an effect, check it, render it, read the picture, improve it.
5. **It can't be broken by editing.** A bad edit produces a useful message, never a crash or a blank screen.

## Non-goals

- Replacing GLSL. A Custom GLSL block stays available as an escape hatch.
- Editing game logic graphs. Those stay on the existing evaluator.
- A timeline or sequencer. Effects read named parameters that a game or film can drive.
- Runtime graph editing in shipped games. Compiled GLSL ships, and the compiler is a development tool.

## Anti-goals

- Users needing to learn GPU concepts to make a first effect.
- A second, separate node system next to the one the engine already has.
- Generated code noticeably slower than hand-written code.
- File formats that churn in version control, or that an AI has to guess at.

## How it works

### The graph

Forge builds on `engine/nodes`: `Graph`, `NodeRegistry`, `GraphEditorModel`, `catalogToJson` and the node editor panel. Existing logic graphs run by interpreting nodes on the CPU. Forge graphs instead **compile to GLSL**, in the same way that M53 anticipated.

`NodeTypeDesc` gains one optional field: how a node writes itself as GLSL. Compiling walks the graph from each stage's outputs, gives every output one local variable, shares repeated work, and hands the result to `GpuParticlesDesc`. The compiler is pure C++, so it can be tested without a GPU.

### Particle target

Three stacks of blocks, run top to bottom:

- **Spawn:** where particles start and how long they live. Sphere, Surface Of Shape, Burst, Rate.
- **Move:** what happens each step. Curl Flow, Gravity, Attract, Stick To Shape, Drag, Follow Path.
- **Look:** how they draw. Colour Over Life, Size Over Life, Stretch With Speed, Glow, Soft Edge, Light The Scene.

A block's values can be set with sliders, gradients and curves, or wired to from nodes. Shapes are their own small node family (Sphere, Capsule, Blend), because signed distance fields are what made the genie and orb possible.

### Surface target

A second target using the same system: nodes that end in a surface output. It compiles to the lit shader's hook, starting from the existing emission hook and extending it to base colour, roughness, normal and displacement. Noise, gradients, Fresnel and flow nodes are shared with the particle target.

### The file

One `.forge.json` per effect, written for people and machines to diff:

```json
{
  "forge": 1,
  "target": "particles",
  "count": 300000,
  "spawn": [{"block": "SphereSurface", "radius": 2.6}],
  "move": [{"block": "CurlFlow", "strength": 3.2, "scale": 0.75}],
  "look": [{"block": "StretchWithSpeed"},
           {"block": "Glow", "color": "#7fb4ff", "intensity": 0.4}],
  "nodes": {"pulse": {"type": "Sine", "speed": 2.0}},
  "wires": [{"from": "pulse.out", "to": "move.0.strength"}]
}
```

Nodes have readable names rather than numeric ids. Editor layout is optional, and it's generated when missing.

### AI support

A headless `forge` command is the main AI interface:

- `forge catalog`: every block and node, with description, ports, ranges, defaults and an example.
- `forge check <file>`: errors as JSON, each with the node, the problem and a suggested fix.
- `forge compile <file>`: the generated GLSL.
- `forge render <file> --at 1,3,5 --out <dir>`: stills, frame capped, so an AI can see its result.

Unknown names get a "did you mean" suggestion. `docs/forge/` holds a guide written for AI agents, with worked examples.

### Editor

A Forge panel in the sandbox editor, using the existing node editor for wires and new stack views for blocks, with a live preview. Saving or editing recompiles in the background, then swaps the effect in. On failure, the last good effect keeps running and the error shows on the block.

### More AAA effects

Added as engine features, each exposed as blocks:

1. **Ribbons and trails,** so lightning and streaks become real continuous strips.
2. **Depth collision,** so particles bounce off and pile on scene geometry.
3. **Sub-emitters,** so one particle's death can spawn others, which sparks and fireworks need.
4. **Lit particles,** shaded by the light volume and casting shadows.
5. **Gas blocks,** feeding Forge effects into the fluid and light volumes.

## Delivery

Each milestone is merged separately and ends with something visible:

- **M74 Forge core:** compiler, particle target, file format, and the headless `forge` tool. The orb is rebuilt as a `.forge.json`.
- **M75 Forge editor:** panel, live editing and starter templates. The 60-second capture happens here.
- **M76 Surface shaders:** the second target.
- **M77 AAA effects:** ribbons, collision, sub-emitters and lit particles, then a new showpiece built entirely in Forge.

## Constraints

- Frame caps stay on, and heavy runs use a power watchdog, until the breaker question is settled.
- Vulkan only. Other games stay pixel-identical and pay nothing unless they use Forge.
- Engine holds mechanisms. Effects belong to games.
- Project conventions apply: code review, tests in Debug, full builds before calling work done.

## Claude's calls

- The name Forge.
- Stacks first, with nodes for depth, rather than nodes everywhere as in Unity.
- Compiling to GLSL, rather than a runtime interpreter, for full GPU speed.
- A command-line tool as the AI interface first. An MCP server could wrap it later.
- Building on the existing node system rather than starting a new one.
- Particles before surface shaders, because they build on the most existing work and rebuild the orb.

## Left to the builder

The exact block list, class names, how nodes describe their GLSL, and the JSON details not shown above, as long as the examples and goals hold.
