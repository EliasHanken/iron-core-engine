# M78: Kindle

A 3 to 5 minute demo game. You are a swarm of about a million sparks inside the Iron Core from the film, cold and dark. You relight it, room by room, then pour yourself into the dormant core, and the whole machine ignites.

It exists to show that Forge drives a real game. The particles are the player, so nothing has to be animated or modelled by an artist.

## Order of work

1. **Flight prototype.** One dark hall with pillars. Fly, squeeze, ignite. Nothing else gets built until this feels great, because the rest of the game depends on it.
2. **Relighting.** Dead surfaces that wake when you ignite near them, with the light spreading outward. Needs sub-emitters and lit particles in Forge.
3. **Three spaces.** Each teaches one thing: flying, squeezing through gaps, and igniting at scale.
4. **The finale.** The dormant core, and one enormous shot the player causes.
5. **A private download.** A packaged Windows build on Cloudflare, behind a sign-in or invite list, so that chosen people can play it.

## What Forge gains

- **Inputs.** `Input` and `InputVector` nodes are values the game sets each frame with `Player::setInput`, like exposed properties in VFX Graph. They use the existing uniform slots, so setting one costs nothing and needs no recompile.
- **Follow and Blast.** Particles chase a moving point at their own pace, which is what makes a comet tail, and get thrown from a point.
- **Room and Pillars shapes.** The world is plain math, in both GLSL and C++, so the player and every particle collide with the same hall. There is no GPU readback.

## Decisions

- **Point lights carry the light across a room.** The light grid only spreads light a few cells, so the swarm also carries three point lights: one at the head and two trailing. The grid stays for the glow in the air.
- **Flight logic stays free of rendering,** in `games/19-kindle/Swarm.h`, so that it can be tested without a GPU.
- **Capture flags fly a scripted route,** so stills and video are repeatable. Only a person can judge how flying feels.
- **Stay frame-capped.** Live at 60 fps, as with every demo since the outages.
