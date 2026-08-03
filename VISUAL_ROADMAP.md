# Visual Engine Roadmap

This roadmap is intentionally step-by-step. The goal is to build the visual stack of a serious game engine viewport without skipping the boring-but-important layers that make later effects reliable.

## Phase 0: Render Correctness

- Define pass order and render state rules.
- Keep opaque geometry depth-tested and depth-writing.
- Keep overlays, outlines, gizmos, and transparent decals depth-tested but not depth-writing unless a pass explicitly needs it.
- Ground imported objects consistently so floor effects do not intersect meshes.
- Add simple visual regression scenes: cube, car, mech, small props, large prop.

Current pass order:

1. Environment/grid
2. Decals/contact shadows
3. Object outlines
4. Opaque objects
5. Gizmos/debug overlays

The current outline implementation uses an inverted hull, so it renders before opaque objects. A later screen-space outline pass can move after opaque rendering once framebuffer/depth texture infrastructure exists.

Current debug views:

- `F1`: final shaded view
- `F2`: albedo/base color
- `F3`: normals
- `F4`: lighting only

## Phase 1: Scene Lighting Foundation

- Represent lights as scene data, not hardcoded shader constants.
- Support directional, point, and spot lights.
- Add light gizmos with editor colors and range indicators.
- Add per-light intensity, color, range, and enabled state.
- Add ambient light controls and a simple environment color.

Current status:

- Scene owns `Light` data.
- Directional, point, and spot variants exist.
- The default scene currently uses one directional light and one point light.
- The active shader is temporarily generic diffuse/specular lighting so light behavior is easy to inspect.
- The default scene includes a lit floor plane and simple planar projected shadows.
- Spot light data/cone support is retained for a later pass, but the next checkpoint should validate the simpler one-directional/one-point setup first.

## Phase 2: Shadows

- Add a depth-only shadow map pass for the main directional light.
- Sample the shadow map in the toon shader.
- Add bias controls, softening, and cascade planning.
- Keep planar/contact shadows as a cheap local fallback.

## Phase 3: Camera And Post-Processing

- Render into an HDR framebuffer instead of directly to the window.
- Add tonemapping, exposure, gamma correction, and color grading.
- Add bloom for emissive signs, light panels, and stylized attack effects.
- Add vignette, subtle film grain/noise, and optional chromatic offset.

## Phase 4: Materials

- Preserve glTF base color, normal, emissive, occlusion, roughness, and metalness.
- Convert PBR textures into stylized material controls.
- Add material presets: painted metal, rubber, glass, cloth, skin, emissive signage.
- Add per-material outline width, rim color, ramp softness, and shadow tint.

## Phase 5: Reflections And Probes

- Add simple reflection cubemap support.
- Add local reflection probes for showroom/urban environments.
- Add screen-space reflections only after framebuffer infrastructure is stable.

## Phase 6: Editor Viewport Polish

- Add camera orbit/pan/zoom controls.
- Add transform gizmos.
- Add object selection outlines separate from art outlines.
- Add scene hierarchy and inspector panels.
- Add visual debug modes: albedo, normals, depth, lighting, shadows, emissive.

Current status:

- Outliner exists for object groups and lights.
- Details panel exists for transforms and light properties.
- Toolbar supports select/move/rotate/scale modes.
- Camera supports orbit, pan, and zoom.
- Basic 3D tool gizmo visuals exist for select, move, rotate, and scale.
- Click selection and interactive center/axis/plane/ring handles exist.
- Gizmo handles now provide hover and active highlighting.
- Rotate rings use a smooth torus mesh and angle-based drag accumulation.
- Polished Unreal-style arrow/cone handle meshes are still pending for move/scale.

## Phase 7: Stylized Rendering Identity

- Add art-directed ramp textures.
- Add screen-space outlines for stable silhouette thickness.
- Add optional ink hatching/halftone overlays.
- Add stylized motion streaks and emissive accent passes.
- Build reference scenes that test characters, vehicles, props, and dense urban assets.
