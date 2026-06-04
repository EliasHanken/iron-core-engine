## Summary

- The sandbox editor used a 1x1 **black** placeholder cubemap (from M29), so there was no real sky and the helmet's reflection sampled black.
- Added `iron::createSunsetSkybox(Renderer&, faceSize=256)` in `engine/render/ProceduralSky.{h,cpp}` — a reusable, backend-agnostic helper that consolidates the `generateSunsetFace` gradient currently copy-pasted inline across 6 games.
- Wired it into `games/11-sandbox` so the editor host now shows the procedural sunset sky (and the helmet reflects it).

## Test plan

- [x] Full suite green (44/44)
- [x] sandbox builds clean
- [ ] Visual: sandbox shows the sunset skybox; helmet reflection picks it up

## Notes / follow-up

- The 6 existing inline copies of `generateSunsetFace` (showcase, net-cubes, net-tag, physics-playground, strandbound, net-shooter) can later be migrated to this helper — left as a separate cleanup.
- Making the skybox a scene-authored property (a `SceneFile` field editable in the Environment panel) is a natural future editor enhancement.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
