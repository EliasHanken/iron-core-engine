## Summary

M42 — CollisionShape + AudioEmitter authorable components. Completes the editor's "place objects, add collision, set audio" loop, exercised in M41 Play mode.

- **Two POD components** added as `std::optional` fields on `iron::SceneEntity`: `CollisionShape` (Box/Sphere/Capsule × Static/Dynamic + size + mass) and `AudioEmitter` (wavPath/gain/loop/spatial/playOnStart). Reflection sidecars register both + their enums.
- **Serialization + Inspector widgets are automatic** via the M38/M39 reflection layer — no `ReflectionIO`/`ReflectionInspector` engine changes (Bool/Float/String/Vec3/Enum all already supported). SceneIO gains `collision`/`audio` keys gated on the optionals (additive; existing scenes load unchanged).
- **Inspector Add/Remove-Component combo** — table-driven over the optional components; generic UI, one row per component.
- **`AudioEngine` looping-voice API** — `VoiceId` + `playLooping` / `stop` / `setVoicePosition`; looping voices reserve a source (excluded from one-shot voice-stealing). One-shot paths unchanged.
- **`PhysicsWorld`** — added static sphere/capsule creators + rotation-at-create (trailing default arg; existing callers unchanged).
- **Sandbox runtime** — on Edit→Play, builds Jolt bodies + audio voices from authored components; dynamic bodies write their pose back into `scene.entities` (the existing scene→World mirror propagates to the renderer) so meshes fall/move; the camera drives the audio listener; looping voices track dynamic positions. Stop tears down bodies/voices; M41's snapshot restores transforms. The M41 magenta debug cube is retired. Edit mode draws green collider wireframes.
- **Tests**: new `test_collision_shape`, `test_audio_emitter`; `test_scene_io` / `test_physics_world` / `test_audio_engine` gained subtests. 52 → 54 CTest entries.

## Test plan

- [x] Full suite green (54/54)
- [x] ironcore + ironcore_editor + sandbox build clean
- [x] Visual: add collider → Play → dynamic mesh falls onto static floor; add looping emitter → positional sound follows the object; Stop restores; Save persists.

## Known v1 limits (intentional, deferred)

- Kinematic bodies; physics materials (restitution/friction); per-emitter rolloff.
- Trigger/sensor volumes + contact events to script.
- Generic `world.add<T>` Add-Component combo (needs World migration).
- Collider auto-fit from mesh bounds (defaults to unit sizes; user resizes in Inspector).
- Capsule wireframe is a cylinder approximation. Many looping emitters can exhaust the 32-source pool (warn-once).
- A `loop=true, spatial=false` emitter has no looping-local API yet, so it plays once (engine limit).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
