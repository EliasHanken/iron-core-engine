## Summary

- Gizmo now renders **always-on-top** of geometry via a new depth-disabled overlay line path (`Renderer::drawLineOverlay` + a second `VkDebugLines` pipeline).
- Handles **highlight on hover**; clicking a highlighted handle grabs it — fixes the M31 bug where clicking a handle deselected the object.
- Gizmo is drawn at the selected entity's **world-AABB center** so it sits on the visible mesh and stays put as the camera pitches.
- Dragging is smooth — the gizmo holds the last axis param on degenerate/grazing solves instead of jumping (fixes the M31 twitch).

## Test plan

- [x] Full suite green (46/46) — engine overlay path is non-breaking
- [x] ironcore + ironcore_editor + sandbox build clean
- [ ] Visual: gizmo visible through geometry; hover highlights; click-grab never deselects; drag smooth; gizmo centered on the mesh

## Known v1 limitations

- Loose world-AABB picking; world-axis gizmos only; no planar handles, snapping, multi-select, or undo.
- Rotate/scale operate about the entity pivot though the gizmo is drawn at the bounds center (visually fine when pivot ≈ center).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
