# ZZZ Style Engine Prototype

Native C++/OpenGL engine-editor prototype for importing meshes and rendering them with a stylized anime/urban look inspired by the broad rendering ingredients in games like Zenless Zone Zero. This project should not copy proprietary models, environments, UI, or exact assets. The goal is a reusable pipeline for user-provided Sketchfab-style assets.

The project originally started as a Rust prototype. The active implementation is now moving to C++ so the engine code is easier for the project owner to read, debug, and extend.

## Current Milestone

- New C++ source tree under `cpp/`.
- CMake build using Visual Studio C++ build tools.
- Native Win32 OpenGL window.
- Component-driven scene skeleton:
  - every placeable item is an actor
  - actors own root scene transforms
  - active components currently include static meshes and lights
  - placeholders exist for future engine/game systems
- Default C++ scene with two cube actors, one directional light actor, and one point light actor.
- C++ renderer draws the world grid, actors, light icons, and local/world-aware gizmos.
- Scene mesh drawing now has a first modern OpenGL path using shader programs and GPU vertex/index buffers.
- Imported/custom mesh parts are cached on the GPU after first use instead of re-uploading every frame.
- The object shader now has a first direct-light metallic/roughness PBR pass with directional and point light contribution plus debug views.
- Materials now carry texture slots for base color, metallic/roughness, normal, and emissive maps.
- OBJ `.mtl` import now records common texture map paths, and the C++ shader samples base-color plus combined metallic/roughness textures when present.
- The viewport work area now uses a light gray editor background as a temporary stand-in until a skybox/environment pass exists.
- Mesh actors now use a first shader-based expanded-backface outline pass instead of the older selected wireframe shortcut.
- Dear ImGui is vendored under `cpp/vendor/imgui` and now drives the C++ editor UI.
- C++ editor controls now include:
  - top toolbar for select/move/rotate/scale, local/world, and snap
  - left outliner with actor selection, primitive creation, directional/point/spot light creation, and deletion
  - right details panel with editable selected actor transform/type/light/mesh/material info
  - right mouse orbit, middle mouse pan, and mouse wheel zoom
  - camera-following editor grid like the Rust prototype
  - click selection for actors/lights
  - mouse-drag move, rotate, and scale handles
  - Home reset
- C++ import currently supports `.obj`, `.gltf`, and `.glb` by command-line argument or drag/drop.
- C++ glTF/GLB import currently covers static triangle meshes, node transforms, vertex normals, UVs, vertex colors, base material factors, external buffers, embedded GLB buffers, external image references, data URI images, and embedded GLB image buffer views.
- C++ glTF/GLB import does not yet cover skins, animation clips, morph targets, cameras, light extensions, or tangent-space normal map shading.

Legacy Rust reference still contains:

- OpenGL window and render loop.
- Drag and drop `.glb`, `.gltf`, or `.obj` files into the window.
- Basic CPU import path for mesh positions, normals, indices, and base material color.
- GPU mesh upload with a direct-light Cook-Torrance/GGX PBR shader and a backface-expanded outline pass.
- Scene architecture uses a single actor list. Actors own root scene components and component lists; current active components are static mesh and light, so everything in the outliner is a selectable actor.
- Engine subsystem placeholders are split into `src/engine.rs`, and shader source is split into `src/shaders.rs`.
- Details-panel material controls for selected mesh actors: metallic and roughness.
- Default lighting test scene with two cube actors, one directional light actor, one point light actor, and a non-scene editor/world grid overlay.
- Scene-owned lights affect object shading through the details panel.
- Viewport outliner/details panels for scene objects, transforms, and lights.
- Outliner creation/deletion for scene primitives and lights:
  - objects: cube, plane, sphere
  - lights: directional, point, spot
- Lights use distinct editor icons and can be selected from the outliner or viewport, then moved/rotated with the active gizmo.
- Toolbar snap toggle for grid-snapped movement on the X/Z plane.
- The grid is a visual editor guide only; it does not receive shadows, block movement, or constrain objects to `Y >= 0`.
- Click selection plus active 3D transform gizmos:
  - move lines with cone arrowheads, center free-move, and two-axis plane handles
  - smooth rotate axis rings with angle-based drag
  - scale axis boxes, center uniform scale, and two-axis plane handles
  - hover/active highlighting for the handle being targeted
  - toolbar toggle for Global/Local transform space
- Style presets:
  - `1`: character-biased palette
  - `2`: street palette
  - `3`: combat palette
  - `F1`: final shaded view
  - `F2`: albedo/base color debug view
  - `F3`: normal debug view
  - `F4`: lighting-only debug view
  - `F5`: roughness debug view
  - `F6`: metallic debug view
  - `Q`: select tool
  - `W`: move tool
  - `E`: rotate tool
  - `R`: scale tool
  - `Delete`: delete selected actor
  - `Snap`: toggle grid snapping while moving
  - `Global`/`Local` toolbar button: toggle transform space
  - `Home`: reset scene
  - Right mouse drag: orbit camera
  - Middle mouse drag: pan camera
  - Mouse wheel: zoom camera
  - Click an object in the viewport to select it
  - Click active gizmo center/axis/plane/ring handles to move, rotate, or scale
  - `Esc`: quit

## Build And Run C++

From PowerShell in this folder:

```powershell
.\build_cpp.bat
```

Then run:

```powershell
.\cpp\build-msvc\ForgeEngine.exe
```

The build script opens the Visual Studio developer environment for you, configures CMake, and builds `ForgeEngine.exe`.

## Run Legacy Rust Prototype

```powershell
cargo run
```

You can also pass a model path:

```powershell
cargo run -- "C:\path\to\model.glb"
```

## Pipeline Target

For the basic PBR renderer checklist and learning notes, see [PBR_PIPELINE.md](PBR_PIPELINE.md).
For the long-term engine/editor architecture notes, see [ENGINE_ARCHITECTURE.md](ENGINE_ARCHITECTURE.md).
For the Rust-to-C++ feature parity ledger, see [RUST_PARITY_AUDIT.md](RUST_PARITY_AUDIT.md).

1. Import model data from `.glb/.gltf/.obj`.
2. Normalize scale and origin so arbitrary Sketchfab assets enter the scene at a predictable size.
3. Extract useful material inputs:
   - base color/albedo
   - normal/tangent data
   - emissive surfaces
   - roughness/metalness as style hints, not strict PBR
4. Convert material data into stylized render parameters:
   - stepped diffuse bands
   - colored shadows
   - directional, point, and spot light contribution
   - graphic specular highlights derived from imported albedo
   - rim-light color and width
   - outline width/color
   - emissive accent intensity
5. Render with separate passes:
   - depth prepass
   - toon base pass
   - inverted-hull or screen-space outlines
   - emissive/bloom accents
   - optional post-process color grade, vignette, and halftone/noise texture
6. Add editor affordances:
   - asset browser
   - object selection
   - translate/rotate/scale gizmos
   - material style controls
   - save/load scene files

## Next Engineering Steps

1. Visual QA textured OBJ materials, the light workspace background, mesh outline pass, C++ PBR shader, and debug views in the app.
2. Add glTF/GLB drag-drop import in C++.
3. Visual QA `.glb/.gltf` imports from Sketchfab-style assets, especially multi-material textured models.
4. Add tangent generation and tangent-space normal maps.
5. Continue the PBR pipeline: shadow maps, HDR, tonemapping, and environment lighting.
