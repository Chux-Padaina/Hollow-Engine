# ZZZ Style Engine Prototype

Small native Rust/OpenGL editor prototype for importing meshes and rendering them with a stylized anime/urban look inspired by the broad rendering ingredients in games like Zenless Zone Zero. This project should not copy proprietary models, environments, UI, or exact assets. The goal is a reusable pipeline for user-provided Sketchfab-style assets.

## Current Milestone

- OpenGL window and render loop.
- Drag and drop `.glb`, `.gltf`, or `.obj` files into the window.
- Basic CPU import path for mesh positions, normals, indices, and base material color.
- GPU mesh upload with a toon ramp shader, rim light, and backface-expanded outline pass.
- Editor grid, two default cubes, and left-mouse drag to use the active transform tool.
- Viewport outliner/details panels for scene objects, transforms, and lights.
- Click selection plus active 3D transform gizmos:
  - move arrows, center free-move, and two-axis plane handles
  - rotate axis rings
  - scale axis boxes, center uniform scale, and two-axis plane handles
  - hover/active highlighting for the handle being targeted
- Style presets:
  - `1`: character-biased palette
  - `2`: street palette
  - `3`: combat palette
  - `F1`: final shaded view
  - `F2`: albedo/base color debug view
  - `F3`: normal debug view
  - `F4`: lighting-only debug view
  - `Q`: select tool
  - `W`: move tool
  - `E`: rotate tool
  - `R`: scale tool
  - `Home`: reset scene
  - Right mouse drag: orbit camera
  - Middle mouse drag: pan camera
  - Mouse wheel: zoom camera
  - Click an object in the viewport to select it
  - Click active gizmo center/axis/plane/ring handles to move, rotate, or scale
  - `Esc`: quit

## Run

```powershell
cargo run
```

You can also pass a model path:

```powershell
cargo run -- "C:\path\to\model.glb"
```

## Pipeline Target

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

1. Split `src/main.rs` into `renderer`, `assets`, `scene`, and `editor` modules.
2. Add texture import and OpenGL texture binding for glTF base color, normal, and emissive maps.
3. Replace cube/bar gizmo primitives with proper arrow, cone, torus, and planar handle meshes.
4. Add multi-object selection and grouped transform pivots.
5. Add a post-processing framebuffer for bloom and color grading.
6. Add a small `.scene.json` format so imported objects and style settings persist.
