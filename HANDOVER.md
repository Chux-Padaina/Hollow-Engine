# Handover

## Goal

Build a portfolio-grade native OpenGL game-engine demo focused on visuals, not full gameplay tooling. The target is a stylized anime/urban rendering pipeline that gets close to the broad visual ingredients of Zenless Zone Zero: punchy toon lighting, bold silhouettes, colored urban light, crisp imported props/vehicles/characters, emissive accents, and a polished editor-style viewport.

Do not copy proprietary ZZZ models, environments, UI, logos, or exact assets. The engine should import user-provided Sketchfab-style assets and convert their available data into this stylized rendering direction.

## Current State

- Rust/OpenGL desktop app.
- Git repo initialized with incremental commits.
- Drag/drop import for `.glb`, `.gltf`, and `.obj`.
- glTF node transforms are applied so multi-part models stay assembled.
- glTF base color factors, vertex colors, UVs, and base color textures are preserved.
- Imported primitives are grouped so dragging moves the whole imported asset.
- Default editor test scene with two cubes, a lit floor plane, an editor grid, one directional light, and one point light.
- Explicit render passes:
  1. Environment/grid
  2. Decals/contact shadows
  3. Object outlines
  4. Opaque objects
  5. Gizmos/debug overlays
- Generic lighting foundation:
  - scene-owned directional lights with color/intensity
  - scene-owned point lights with position, color, range, intensity
  - spot light data/cone support retained for later, but the default scene currently uses one directional and one point light
- Generic object shader:
  - ambient term
  - directional Lambert diffuse and Blinn specular
  - point-light Lambert diffuse and Blinn specular
  - simple planar projected shadows onto the floor plane
  - simple fog and tonemapping
  - inverted-hull outline
- Debug object views:
  - `F1`: final
  - `F2`: albedo
  - `F3`: normals
  - `F4`: lighting only
- Editor UI:
  - outliner with object groups and lights
  - details panel for object transforms and light properties
  - toolbar for select/move/rotate/scale tools
  - orbit/pan/zoom editor camera controls
  - simple tool-specific 3D gizmo visuals for select/move/rotate/scale
  - object click-selection in the viewport
  - clickable transform gizmo handles for center, axis, plane, and ring manipulation
  - hover/active highlighting on targeted gizmo handles
  - smooth torus rotation rings with cursor-angle-based drag accumulation

## Recent Fixes

- Fixed import explosion by preserving glTF hierarchy transforms.
- Fixed texture/color preservation for glTF base colors.
- Fixed oversized outlines by switching outline expansion to a pixel-based clip-space offset.
- Fixed abrupt ramp changes by replacing hard `step()` bands with smooth stylized bands.
- Fixed render ordering and hidden OpenGL state dependencies after pass refactor.
- Replaced showroom with a simpler grid-based default scene so editor work and scene-building tests are easier to read.
- Added Unreal-like transform affordances: move arrows, rotate rings, scale endpoint boxes, two-axis plane handles, and visible hover/active feedback.
- Replaced pixelated box-built rotation rings with a generated torus mesh and fixed rotation drags so rings can rotate through larger angles intuitively.
- Temporarily reset object lighting to a generic diffuse/specular shader, added a lit floor plane, simplified the default scene to one directional and one point light, and made shadow decals project from the directional light.

## Known Limitations

- No shadow maps yet; current shadows are simple planar projected decals for lighting validation.
- No HDR framebuffer or real post-processing yet.
- No bloom despite emissive-looking showroom panels.
- No area lights.
- No reflection probes.
- No normal/emissive/roughness/metallic texture support yet.
- Editor panels and clickable transform gizmos exist. Gizmo behavior covers the core Unreal-style center/axis/plane/ring workflow, but move/scale visuals still use cube/bar primitives rather than proper arrow/cone meshes.
- Inverted-hull outlines can still be imperfect on thin/open geometry; screen-space outlines should come later.
- Scene save/load is not implemented yet, so editor changes are session-only.

## Next Step

Finish the editor interaction foundation by checking cube, Porsche, and mech transforms with select/move/rotate/scale tools. Once the gizmo behavior feels reliable, move into the next visual-engine layer:

1. Add proper gizmo meshes and depth-aware selection outlines.
2. Add light range visualization in the viewport.
3. Prepare scene save/load so test compositions can persist.
4. Prepare for the first shadow-map pass.

## Project Taste Direction

Favor a crisp, confident, stylized viewport over physically perfect realism. The current shader is intentionally generic so lighting and shadows can be validated clearly before reintroducing ZZZ-adjacent toon bands, colored shadows, graphic rim light, and post-processing.
