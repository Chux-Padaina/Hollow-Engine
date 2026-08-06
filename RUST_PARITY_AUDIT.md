# Rust Prototype Parity Audit

This file tracks what exists in the Rust prototype and whether it has been ported to the active C++ implementation.

## Ported Or Mostly Ported

- Native window and render loop.
- Actor-centric scene model.
- Root scene transform per actor.
- Component-driven actors for static meshes and lights.
- Placeholder engine subsystem/component slots.
- Default two-cube scene.
- Directional, point, and spot light actor data.
- Select, move, rotate, and scale editor tools.
- Global/local transform-space toggle.
- Snap-to-grid movement toggle.
- Outliner-style actor list.
- Add object controls: cube, plane, sphere.
- Add light controls: directional, point, spot.
- Delete selected actor.
- Home reset.
- Right-mouse camera orbit, middle-mouse pan, mouse-wheel zoom.
- Camera-following visual editor grid.
- Selected actor details panel layout.
- ImGui-backed editable transform, light, and material controls.
- Rust-style toolbar labels for tool, view, and style.
- Style preset state: Character, Street, Combat.
- Debug view state: Final, Albedo, Normals, Lighting, Roughness, Metallic.
- Status text.
- Basic light icons and transform gizmo rendering.
- Basic click selection for actors and lights.
- Basic mouse-driven transform gizmo interaction.
- Command-line OBJ import.
- Drag/drop OBJ import.
- OBJ material diffuse color import from `.mtl` files.
- Mesh normalization after import.
- Custom imported mesh part rendering.
- First C++ shader/VBO path for scene meshes.
- Persistent GPU cache for imported/custom mesh parts.
- Cook-Torrance/GGX direct-light PBR shader for C++ scene meshes.
- Directional and point light contribution in the C++ object shader.
- Albedo/normal/lighting/roughness/metallic debug rendering for C++ scene meshes.
- Light gray editor workarea background.
- Material texture slots for base color, metallic/roughness, normal, and emissive maps.
- OBJ `.mtl` base-color texture path import.
- Base-color texture loading and sampling in the C++ object shader.
- Combined metallic/roughness texture loading and sampling in the C++ object shader.
- Static `.gltf`/`.glb` import in C++ by command-line argument and drag/drop.
- glTF node transform preservation for imported static meshes.
- glTF base color, metallic, and roughness factor import.
- glTF normal texture path and normal strength import.
- glTF vertex color and UV import.
- glTF external buffers, embedded GLB buffer chunks, external image references, data URI images, and embedded image buffer views.
- Tangent generation for imported/custom meshes.
- Tangent-space normal map sampling in the C++ object shader.
- Material normal-strength editing in the details panel.
- First directional-light shadow map pass.
- Toolbar toggle for real shadow-map sampling.

## Partially Ported

- Editor UI polish:
  - Rust uses egui panels with a slightly more mature retained layout.
  - C++ now uses Dear ImGui with editable controls, but the panels still need visual QA and finer layout tuning.
- Transform gizmos:
  - Rust has center, axis, plane handles, smooth rings, hover/active highlighting, ray-based picking, and more careful drag math.
  - C++ has center/axis/ring interaction and improved screen-space dragging, but lacks plane handles, hover highlight, polished arrows, and ray-accurate selection.
- Renderer pass structure:
  - Rust has explicit environment, opaque, outlines, and gizmo passes with shader state.
  - C++ has the same conceptual order and now draws scene meshes and actor outlines through shaders/VBOs, but editor grid/gizmos/light icons still use immediate-mode bootstrap rendering.
- Outlines:
  - Rust has a more mature expanded-backface outline pass.
  - C++ now has the same first-pass outline technique for mesh actors, but it still uses object-space expansion rather than a final screen-pixel-stable outline.
- Grid:
  - Rust uses a shader-driven grid mesh with camera-following origin.
  - C++ now follows the camera but is still line-rendered.
- Material/display state:
  - Rust has metallic/roughness sliders and debug views connected to shader uniforms.
  - C++ now renders material factors, texture-backed base color, texture-backed combined metallic/roughness maps, and texture-backed tangent-space normal maps in the object shader.

## Not Yet Ported

- glTF skins and skeletal animation.
- glTF morph targets.
- glTF camera import.
- glTF light extension import.
- Spot lighting in shader.
- Cascaded or production-quality shadow maps.
- Point and spot light shadow maps.
- Screen-pixel-stable outline widths.
- Smooth stylized lighting bands.
- Fog and tonemapping.
- Rust shader source split/parity in C++.
- Ray-based mesh picking using bounds radius.
- Ray-based light picking.
- Ray-based plane handles.
- Two-axis transform plane handles.
- Hover/active gizmo highlighting.
- Polished arrowhead gizmo meshes.
- Torus rotation gizmo mesh.
- Per-light animated/pulsing intensity helpers.
- Light packets/count helpers for shader upload.
- Selected-object-specific outline/highlight treatment matching Rust.
- Texture/material inspector editing.
- Camera details editing.
- Scene status messages for import errors.

## Recommended Port Order

1. Visually QA the Dear ImGui panels against the Rust egui layout and remove the unused hand-drawn overlay path.
2. Visual QA the C++ PBR shader and debug views, then move editor gizmos/grid to the modern path.
3. Port glTF import and texture preservation.
4. Port Rust render passes and remaining shader uniforms.
5. Port spot lighting and outline shader parity.
6. Port outline pass and polished gizmo meshes.
7. Continue the planned full PBR pipeline: metallic-roughness textures, normal maps, shadow maps, HDR, and environment lighting.
