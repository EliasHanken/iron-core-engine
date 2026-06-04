## Summary

- Click an entity in the sandbox viewport to select it (ray-vs-AABB pick); selection is shared with the M30 Inspector/Outliner.
- Translate / rotate / scale gizmos with W / E / R mode switching; drag a handle to transform the selected entity live.
- Engine picking helpers (`Mat4 inverse`, `meshBounds`, `screenPointToRay`, `pickEntity`) added with unit tests; a `Gizmo` controller in `ironcore_editor` renders via the existing debug-line system (no new pipeline).

## Test plan

- [x] `test_mesh_bounds` + `test_picking` (Mat4 inverse round-trip, screen-ray, ray-vs-AABB nearest) — full suite 46/46 green
- [x] ironcore_editor + sandbox build clean
- [ ] Visual: click-select syncs with the inspector; W/E/R + drag transform live; hold-RMB still flies

## Known v1 limitations

- Loose world-AABB picking (rotated objects select via an enlarged box; thin plane given a tiny Y thickness to stay pickable).
- World-axis gizmos only; no planar two-axis handles, snapping, multi-select, or undo.
- Gizmo depth follows the debug-line pass (may be occluded by geometry / drawn over panels where they overlap); always-on-top is a follow-up.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
