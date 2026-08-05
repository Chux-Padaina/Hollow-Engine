# Handover

## Goal

Build a portfolio-grade native OpenGL game-engine demo focused on visuals, not full gameplay tooling. The target is a stylized anime/urban rendering pipeline that gets close to the broad visual ingredients of Zenless Zone Zero: punchy toon lighting, bold silhouettes, colored urban light, crisp imported props/vehicles/characters, emissive accents, and a polished editor-style viewport.

Do not copy proprietary ZZZ models, environments, UI, logos, or exact assets. The engine should import user-provided Sketchfab-style assets and convert their available data into this stylized rendering direction.

## Current State

- Active migration from Rust/OpenGL to C++/OpenGL.
- New C++ app builds successfully with Visual Studio build tools through `build_cpp.bat`.
- C++ source tree lives under `cpp/`.
- Current C++ app has a native Win32 OpenGL window, a component-driven scene skeleton, two cube actors, one directional light actor, one point light actor, camera-following world-grid rendering, light icons, a Rust-inspired editor overlay, local/world-aware gizmos, mouse camera controls, actor selection, primitive/light add controls including spot lights, deletion, Home reset, and mouse-driven move/rotate/scale.
- Rust/OpenGL desktop app remains in the repo as the feature-rich reference while the C++ version catches up.
- Git repo initialized with incremental commits.
- `PBR_PIPELINE.md` tracks the render-pipeline learning checklist and implementation plan.
- `ENGINE_ARCHITECTURE.md` tracks the long-term editor/engine skeleton.
- `RUST_PARITY_AUDIT.md` tracks what has and has not been ported from the Rust prototype.
- `src/engine.rs` owns placeholder engine subsystem slots.
- `src/shaders.rs` owns GLSL shader source strings.
- Drag/drop import for `.glb`, `.gltf`, and `.obj`.
- glTF node transforms are applied so multi-part models stay assembled.
- glTF base color factors, vertex colors, UVs, and base color textures are preserved.
- Scene data uses one actor list, similar to the Unreal editor mental model: every outliner item is an `Actor`; actors own root scene components and component lists, and current active components cover static meshes and lights.
- Imported glTF/OBJ assets become one mesh actor with multiple mesh parts, so dragging moves the whole imported asset.
- Default editor test scene with two cube actors, a non-scene editor/world grid overlay, one directional light actor, and one point light actor.
- The old baked-in floor object has been removed. A floor/plane can now be added as a normal scene object from the outliner.
- Explicit render passes:
  1. Editor/world grid guide
  2. Opaque mesh actors
  3. Object outlines
  4. Gizmos/debug overlays
- Generic lighting foundation:
  - scene-owned directional lights with color/intensity
  - scene-owned point lights with position, color, range, intensity
  - spot light data/cone support retained for later, but the default scene currently uses one directional and one point light
- Direct-light PBR object shader:
  - Cook-Torrance/GGX specular BRDF
  - Smith-Schlick visibility
  - Schlick Fresnel
  - Lambert diffuse energy split for non-metals
  - per-mesh metallic and roughness factors
  - details-panel sliders for selected mesh actor metallic and roughness
  - simple fog and tonemapping
  - inverted-hull outline
- Debug object views:
  - `F1`: final
  - `F2`: albedo
  - `F3`: normals
  - `F4`: lighting only
  - `F5`: roughness
  - `F6`: metallic
- Editor UI:
  - `src/editor.rs` owns the toolbar, outliner, details panel, and small egui widgets
  - outliner with one actor list
  - add/delete controls for cube, plane, sphere, directional light, point light, and spot light
  - distinct viewport icons for directional, point, and spot lights
  - viewport and outliner light selection
  - transform gizmos for selected lights; movement changes position and rotation changes direction
  - toolbar snap toggle for X/Z grid-snapped movement
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
- Implemented the first PBR milestone: direct-light Cook-Torrance/GGX shading with per-mesh metallic/roughness factors and glTF metallic/roughness factor import.
- Added selected mesh actor material sliders for metallic/roughness and debug views for roughness/metallic masks.
- Split editor UI code into `src/editor.rs`, removed the default floor, and added outliner creation/deletion for primitives and lights.
- Added light actor icons, light selection/gizmo transforms, grid snap toggle, larger camera-following editor grid, corrected generated sphere triangle winding, and moved outlines after opaque rendering so depth clipping is more reliable.
- Reworked scene ownership so meshes and lights are both component-driven actors in one outliner list, removed the fake contact-shadow pass from the grid, and removed all Y=0 movement locking.
- Added root scene components, component lists, static mesh components, light components, and placeholder component types for scripts, audio, animation, input, camera, collision, physics, skeletal meshes, UI, particles, nav/AI, post-process volumes, and network replication.

## Known Limitations

- C++ implementation is not yet feature-equivalent with the Rust prototype.
- C++ rendering currently uses dependency-light immediate-mode OpenGL as a bootstrap layer. It should move to shader/VBO-based modern OpenGL before the PBR pipeline is ported.
- C++ editor panels are currently a lightweight OpenGL overlay, not egui/ImGui. They visually restore the editor shell but do not yet support editable numeric fields/sliders.
- C++ drag/drop import, glTF import, material inspector sliders, style/debug view buttons, and full Unreal-quality gizmo hit testing are pending.
- C++ has a camera-following visual grid now, but it is still immediate-mode line rendering rather than the Rust shader grid with subtle animated/stylized treatment.
- No shadow maps yet; the previous fake planar/contact shadow pass is currently disabled/removed so the editor grid stays purely visual.
- No HDR framebuffer or real post-processing yet.
- No bloom despite emissive-looking showroom panels.
- No area lights.
- No reflection probes.
- No normal/emissive/roughness/metallic texture support yet.
- Editor panels and clickable transform gizmos exist. Gizmo behavior covers the core Unreal-style center/axis/plane/ring workflow, but scale visuals still use cube/bar primitives rather than proper polished handle meshes.
- Inverted-hull outlines can still be imperfect on thin/open geometry; screen-space outlines should come later.
- Scene save/load is not implemented yet, so editor changes are session-only.
- Grid snapping currently affects X/Z movement only. It does not snap rotation or scale yet, and it does not prevent actors from moving below the grid.
- Light scale gizmo changes local light range; directional light scale is intentionally ignored.
- Code is only partially split so far. Editor UI is separated; renderer, import, scene, mesh, and shader code still need their own modules.

## Next Step

Finish the C++ migration foundation, then move back into the basic PBR pipeline described in `PBR_PIPELINE.md`:

1. Replace the lightweight C++ overlay with Dear ImGui or another robust editor UI layer.
2. Replace bootstrap immediate-mode rendering with modern OpenGL buffers and shaders.
3. Port/import glTF mesh/material loading.
4. Rebuild direct-light Cook-Torrance/GGX PBR.
5. Continue metallic-roughness textures, normal maps, shadow maps, HDR, and environment lighting.

## Project Taste Direction

Favor a crisp, confident, stylized viewport over physically perfect realism. The current shader is intentionally generic so lighting and shadows can be validated clearly before reintroducing ZZZ-adjacent toon bands, colored shadows, graphic rim light, and post-processing.
