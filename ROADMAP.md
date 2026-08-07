# Hollow Engine — Roadmap

This tracks where Hollow Engine is now and what's planned next. For how the current system works, see [ARCHITECTURE.md](ARCHITECTURE.md).

## v0.1 (current)

A complete first pass end to end: glTF/GLB/OBJ import, a real metallic-roughness PBR pipeline (multi-light shading, cascaded + spot shadows, IBL, SSAO, HDR post-processing), and a themed Dear ImGui editor with ray-accurate picking, gizmos, and a UI that scales and resizes with the window. Detailed in [ARCHITECTURE.md](ARCHITECTURE.md) and the feature list in [README.md](README.md).

## Near-term

**Editor**
- Real ImGui docking (movable, rearrangeable, tabbable panels) instead of the current fixed toolbar/outliner/details layout. The vendored ImGui version supports it; it isn't wired into this project's backends yet.
- Undo/redo for transform edits, actor creation/deletion, and material changes.
- Multi-select (currently one actor at a time) and basic prefab/instance duplication.
- Per-part material editing — material edits on a multi-part mesh currently apply to every part of the actor's mesh group at once, not the individually selected part.
- An asset browser/registry for imported meshes and textures, instead of import-only-by-drag-and-drop.
- Scene save/load to a file format, rather than only the in-memory default/imported scene.

**Rendering**
- Every enabled shadow-casting light gets its own shadow map, not just the first enabled directional/point/spot light — the current single-shadow-owner setup is the biggest correctness gap in the shadow system.
- Point-light shadows brought up to parity with the directional/spot paths (the six-face cube pass is the oldest, least-maintained of the three) or replaced with a more scalable approach if the light count grows.
- Pixel-accurate picking (per-triangle or per-OBB) as an option once bounding-sphere picking's over-selection at silhouette edges becomes a real workflow problem, plus hover highlighting on individual gizmo handles.
- Texture-based 3D LUT color grading as an alternative to the current parametric contrast/saturation/temperature controls, for more expressive non-parametric grades.
- A closer-to-reference FXAA pass (per-edge length search, sub-pixel aliasing term) or an MSAA/TAA path, since the current FXAA is a simplified single-pass approximation.

## Later / exploratory

- Skinned mesh and animation clip import (glTF import currently covers static triangle meshes only — no skins, morph targets, or animation).
- Area lights and a more physically-based light unit system (current lights are point/directional/spot with an artist-facing intensity float, not photometric units).
- Basic physics or collision placeholders becoming real systems — `PlaceholderComponent` already reserves the slot in the scene graph.
- Cross-platform support. The engine is Windows-only today (Win32 + WGL windowing, WIC texture decoding); a GLFW/SDL + stb_image path would be the natural way to open this up, at the cost of the hand-rolled Win32 layer's directness.
- Repo polish for the public GitHub listing: sample scenes, in-README screenshots/GIFs of the renderer and editor in action, and a license file — none of this is code work, just packaging, and is easiest to do once the current UI/rendering state is final enough to screenshot.
