# CC0 PBR Asset Packs

Real-world PBR textures from [Polyhaven](https://polyhaven.com), released under CC0
(public domain). Each pack contains three 1k PNGs:

- `diffuse.png` — base colour, sRGB
- `normal.png` — tangent-space normal map (OpenGL convention: +Y is up)
- `roughness.png` — Polyhaven roughness convention (1=matte, 0=mirror)

The engine's specular path expects bright=shiny, so use `iron::loadRoughnessAsSpec`
when uploading the roughness PNG; it inverts R/G/B at load time.

Packs:
- `wood/` — wood planks
- `metal/` — metal plate
- `brick/` — red brick
- `ground/` — mud/forest floor

Per-pack `CREDITS.txt` records the exact source asset.
