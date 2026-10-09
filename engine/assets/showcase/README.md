# Showcase assets

This is a deliberately small, import-ready selection for composing Hollow Engine screenshots. Drag any `.glb` file onto the editor window (or use File > Import Model).

Keep each model beside its accompanying `Textures/` folder. Although these assets use the `.glb` container, their color images are referenced as external files rather than embedded in the GLB.

## Dungeon set

The files in `dungeon/` are compatible pieces from Kenney's **Modular Dungeon Kit 2.1**:

- `room-large-variation.glb` — fastest way to get a complete environment shot
- `corridor.glb`, `corridor-corner.glb`, `corridor-junction.glb` — modular layout pieces
- `gate.glb`, `gate-door.glb` — strong foreground and silhouette props
- `stairs-wide.glb` — adds height and a useful leading line to compositions

Suggested first shot: import `room-large-variation.glb`, add one warm point or spot light inside the room and keep a cooler directional/IBL fill. Hide the grid and gizmos for the final capture.

## Characters

- `characters/adventurer.glb` — human-scale subject for an environment shot
- `characters/robot.glb` — alternate subject with a cleaner, more reflective palette

Both are skinned and carry animation clips: imported, they get an **Animator** component that plays the first clip (pick another by name in the inspector).

Suggested second shot: use the adventurer near the gate or at the top of the stairs. Keep the character off-center and use the corridor or stair edges as leading lines.

## Scale note

Models import at their authored size and pivot (1 unit = 1 meter), so the dungeon pieces snap together on a 4-unit grid and the characters stand about 2.7 units tall. Turn on snapping (`G`, step size in the Edit menu) to line pieces up. `engine/assets/scenes/dungeon_showcase.hscene` is a ready-made composition.

## License and source

Both selections are CC0 and may be used in personal and commercial projects. The original license text is preserved in each asset folder.

- Dungeon: <https://kenney.nl/assets/modular-dungeon-kit>
- Characters: <https://kenney.nl/assets/blocky-characters>
