# Handover

## Goal

Build a portfolio-grade native OpenGL game-engine demo focused on visuals, not full gameplay tooling. The target is a stylized anime/urban rendering pipeline that gets close to the broad visual ingredients of Zenless Zone Zero: punchy toon lighting, bold silhouettes, colored urban light, crisp imported props/vehicles/characters, emissive accents, and a polished editor-style viewport.

Do not copy proprietary ZZZ models, environments, UI, logos, or exact assets. The engine should import user-provided Sketchfab-style assets and convert their available data into this stylized rendering direction.

## Current State

- Rust/OpenGL desktop app.
- Git repo initialized with incremental commits.
- `PBR_PIPELINE.md` tracks the render-pipeline learning checklist and implementation plan.
- Drag/drop import for `.glb`, `.gltf`, and `.obj`.
- glTF node transforms are applied so multi-part models stay assembled.
- glTF base color factors, vertex colors, UVs, and base color textures are preserved.
- Imported primitives are grouped so dragging moves the whole imported asset.
- Default editor test scene with warm/cool colored cubes, a darker flattened gray cube floor, a non-scene editor/world grid overlay, one directional light, and one point light.
- Explicit render passes:
  1. Editor/world grid guide
  2. Object outlines
  3. Opaque objects
  4. Decals/contact shadows
  5. Gizmos/debug overlays
- Generic lighting foundation:
  - scene-owned directional lights with color/intensity
  - scene-owned point lights with position, color, range, intensity
  - spot light data/cone support retained for later, but the default scene currently uses one directional and one point light
- Generic object shader:
  - ambient term
  - directional Lambert diffuse and Blinn specular
  - point-light Lambert diffuse and Blinn specular
  - simple planar projected shadows onto the floor cube
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
  - Global/Local transform-space toggle for gizmo orientation and constrained handles
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
- Temporarily reset object lighting to a generic diffuse/specular shader, added warm/cool test cubes and a darker flattened gray cube floor, simplified the default scene to one directional and one point light, made shadow decals project from the directional light, and moved the grid into a non-scene world overlay.
- Added a Global/Local transform-space toggle; constrained gizmo handles now draw, pick, and move in the selected space, while clicking the selected object body uses the active tool's free transform behavior.
- Fixed constrained rotation by applying quaternion deltas around the displayed gizmo axis instead of directly editing Euler components; move gizmos now use cone arrowheads.

## Known Limitations

- No shadow maps yet; current shadows are simple planar projected decals for lighting validation.
- No HDR framebuffer or real post-processing yet.
- No bloom despite emissive-looking showroom panels.
- No area lights.
- No reflection probes.
- No normal/emissive/roughness/metallic texture support yet.
- Editor panels and clickable transform gizmos exist. Gizmo behavior covers the core Unreal-style center/axis/plane/ring workflow, but scale visuals still use cube/bar primitives rather than proper polished handle meshes.
- Inverted-hull outlines can still be imperfect on thin/open geometry; screen-space outlines should come later.
- Scene save/load is not implemented yet, so editor changes are session-only.

## Next Step

Move into the basic PBR pipeline described in `PBR_PIPELINE.md`:

1. Implement direct-light Cook-Torrance/GGX PBR with uniform material values.
2. Add material sliders for metallic and roughness.
3. Add roughness and metallic debug views.
4. Import glTF metallic/roughness factors and textures.
5. Add tangent support and normal mapping.
6. Replace planar shadows with a directional shadow map.

## Project Taste Direction

Favor a crisp, confident, stylized viewport over physically perfect realism. The current shader is intentionally generic so lighting and shadows can be validated clearly before reintroducing ZZZ-adjacent toon bands, colored shadows, graphic rim light, and post-processing.
