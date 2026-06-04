## Summary

- `SceneOutliner` gains an add bar (`+ Cube` / `+ Plane` / glTF path + `+ glTF`, `Duplicate`, `Delete`) and returns the user's intent (`SceneOutliner::Result`); the sandbox host executes it.
- Add / duplicate build a `SceneEntity`, append it, resolve it (new `resolveEntity` helper, shared with the startup load), and select it; delete erases the entity + its resolved entry and reindexes `resolved[].entityIndex`.
- New entities spawn ~5 units in front of the camera, auto-named, and selected with the gizmo. Delete key + Ctrl+D shortcuts. Save persists via `saveSceneFile`.

## Test plan

- [x] Full suite green (46/46) — placement logic is host-side / editor-UI, no engine test touched
- [x] ironcore_editor + sandbox build clean
- [ ] Visual: add cube/plane/glTF (bad path warns); duplicate offsets a copy; delete removes + keeps selection correct; Save persists; relaunch loads the new scene

## Known v1 limitations

- No undo/redo, multi-select, parenting, or asset browser (glTF added by path field).
- Deleting a glTF entity leaks its GPU mesh (no destroyMesh API yet).
- glTF paths resolve relative to the executable directory (same as M29).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
