# Forge Engine Handover

## Project Goal

Build a portfolio-grade native C++/OpenGL engine-editor prototype focused on production-quality rendering foundations and, later, a stylized anime/urban look inspired by the broad visual ingredients of Zenless Zone Zero.

Important boundary: do not copy proprietary ZZZ models, environments, UI, logos, or exact assets. The point is to build an engine pipeline that can import user-provided Sketchfab-style assets and render them with a similar class of high-contrast stylized graphics: crisp silhouettes, strong material response, bold light direction, colored urban lighting, emissive accents, post-processing, and eventually toon/stylized controls layered over a trustworthy PBR base.

## Current Implementation

The active engine is now the C++ implementation under `cpp/`.

The original Rust prototype still exists under `src/` as reference code, but current development should happen in C++ unless explicitly asked otherwise.

Current C++ features:

- Native Win32 OpenGL window.
- CMake build through Visual Studio C++ build tools.
- Dear ImGui editor UI vendored in `cpp/vendor/imgui`.
- Actor/component scene model:
  - every placeable item is an `Actor`
  - actors own a root scene transform
  - active components include static meshes and lights
  - placeholder component/system slots exist for later scripts, cameras, input, animation, audio, UI, post-process volumes, etc.
- Default scene:
  - two cube actors
  - one directional light actor
  - one point light actor
  - no permanent floor object
  - grid is editor-only visual guidance
- Editor tools:
  - outliner actor selection
  - add cube/plane/sphere
  - add directional/point/spot lights
  - delete selected actor
  - details panel for transform, light, mesh, and material controls
  - select/move/rotate/scale tools
  - local/world transform-space toggle
  - grid snap toggle for movement
  - Unreal-like camera controls: RMB orbit, MMB pan, wheel zoom
  - click selection for actors/lights
  - clickable gizmos for movement/rotation/scale
- Import:
  - `.obj`, `.gltf`, and `.glb`
  - command-line argument or drag/drop
  - glTF node transforms preserved
  - static triangle meshes
  - normals, UVs, vertex colors
  - base color factors
  - metallic/roughness factors
  - normal strength
  - external buffers
  - embedded GLB buffers
  - external image references
  - data URI images
  - embedded GLB image buffer views

## Current Rendering Pipeline

The C++ renderer currently has:

- Modern OpenGL shader/VBO path for scene meshes.
- GPU cache for imported/custom mesh parts.
- Material texture slots for:
  - base color
  - metallic/roughness
  - normal
  - emissive storage, not fully shaded yet
- Windows WIC texture loading.
- OBJ `.mtl` texture path import.
- Base-color texture sampling.
- Combined glTF metallic/roughness texture sampling:
  - green = roughness
  - blue = metallic
- Tangent generation for OBJ/glTF/custom meshes.
- Tangent-space normal map sampling.
- Direct-light metallic/roughness PBR shader:
  - Cook-Torrance/GGX distribution
  - Smith-Schlick visibility
  - Schlick Fresnel
  - Lambert diffuse for non-metals
  - directional and point light contribution
- Shadow maps:
  - first enabled directional light casts shadows
  - first enabled point light casts shadows through six depth maps
  - toolbar Shadows toggle
- HDR:
  - RGBA16F scene-color framebuffer
  - ACES-style tonemapping
  - exposure slider
  - debug views bypass artistic tonemap
- Debug views:
  - final
  - albedo
  - normals
  - lighting-only
  - roughness
  - metallic
- Mesh actor outline pass:
  - expanded-backface shader outline
  - still object-space, not screen-pixel-stable yet
- Explicit renderer pass order:
  - shadow maps
  - scene color target setup
  - editor grid
  - actor outlines
  - opaque geometry and light icons
  - tonemap
  - editor overlay/gizmos

The renderer was recently refactored so `Renderer::render` creates a `RenderFrameContext` and dispatches named `RenderPassKind` passes. This is the first architectural step toward a real render graph.

## Current Limitations

Rendering:

- Multi-light shader path is not implemented yet; it still uses the first directional and first point light for real shading/shadows.
- Spot light data exists in the editor/scene model, but spot lighting is not shaded yet.
- Shadows are first-pass only:
  - no cascaded shadow maps
  - no shadow atlas
  - no per-light quality settings
  - no production-grade bias controls
  - point shadows may need seam/bias polish
- No environment lighting / IBL yet.
- No skybox yet.
- No emissive shading or bloom yet.
- No ambient occlusion texture support yet.
- No SSAO/GTAO yet.
- No reflection probes, planar reflections, or SSR yet.
- No color grading/LUT stack yet.
- No volumetric fog yet.
- No real render graph/resource lifetime system yet.

Import/materials:

- glTF skins, skeletal animation, morph targets, cameras, and light extensions are not implemented.
- Authored glTF tangents are not preserved yet; tangents are generated.
- Emissive and occlusion texture import/storage need to be finished in shading.
- Alpha modes and transparent rendering are not yet handled.
- Texture color-space handling needs to become explicit: sRGB for albedo/emissive, linear for normal/roughness/metallic/AO.

Editor/engine:

- Scene save/load is not implemented.
- Asset drawer/asset registry is not implemented.
- Gizmos work but still need production polish and more precise picking.
- Renderer is still mostly contained in `Renderer.cpp`; the recent pass split is only the first structural step.
- Editor grid/gizmos/light icons still use immediate-mode bootstrap drawing.

## Folder Contents

Top-level files:

- `README.md`: main project overview, build/run instructions, current milestone.
- `HANDOVER.md`: this handover document.
- `CONTINUE_PROMPT.md`: prompt to paste into a fresh Codex thread.
- `ENGINE_STATUS_SHORT.md`: short goal/progress README.
- `PBR_PIPELINE.md`: PBR pipeline roadmap and learning notes.
- `ENGINE_ARCHITECTURE.md`: engine/editor architecture notes.
- `RUST_PARITY_AUDIT.md`: Rust-to-C++ feature parity checklist.
- `VISUAL_ROADMAP.md`: visual direction roadmap.
- `build_cpp.bat`: configures/builds the C++ app.
- `Cargo.toml`, `Cargo.lock`: legacy Rust prototype project files.

Active C++ tree:

- `cpp/CMakeLists.txt`: C++ build configuration.
- `cpp/include/Forge/App.hpp`: app/frame-loop public interface.
- `cpp/include/Forge/EditorUi.hpp`: Dear ImGui editor UI interface.
- `cpp/include/Forge/Engine.hpp`: editor/render settings and system registry.
- `cpp/include/Forge/Importer.hpp`: OBJ/glTF import interface.
- `cpp/include/Forge/Json.hpp`: lightweight JSON parser.
- `cpp/include/Forge/Math.hpp`: vectors, matrices, transforms.
- `cpp/include/Forge/Renderer.hpp`: renderer types, resources, frame context, pass declarations.
- `cpp/include/Forge/Scene.hpp`: actors/components/materials/scene model.
- `cpp/include/Forge/TextureLoader.hpp`: texture loading interface.
- `cpp/include/Forge/Win32Window.hpp`: native window wrapper.
- `cpp/src/App.cpp`: main application loop, input, drag/drop, editor integration.
- `cpp/src/EditorUi.cpp`: toolbar, outliner, details panel.
- `cpp/src/Engine.cpp`: settings/system names.
- `cpp/src/Importer.cpp`: OBJ/glTF/GLB import.
- `cpp/src/Json.cpp`: JSON parser implementation.
- `cpp/src/Main.cpp`: C++ entry point.
- `cpp/src/Math.cpp`: math implementation.
- `cpp/src/Renderer.cpp`: OpenGL renderer, shaders, render passes.
- `cpp/src/Scene.cpp`: actor/component scene implementation.
- `cpp/src/TextureLoader.cpp`: WIC texture loading.
- `cpp/src/Win32Window.cpp`: Win32 window/input/drop handling.
- `cpp/vendor/imgui`: vendored Dear ImGui.

Legacy Rust reference:

- `src/main.rs`: Rust app prototype.
- `src/editor.rs`: Rust editor UI reference.
- `src/engine.rs`: Rust engine/system skeleton reference.
- `src/shaders.rs`: Rust shader-source reference.

## Build And Run

From PowerShell in the project folder:

```powershell
.\build_cpp.bat
```

Then run:

```powershell
.\cpp\build-msvc\ForgeEngine.exe
```

Optional model path:

```powershell
.\cpp\build-msvc\ForgeEngine.exe "C:\path\to\model.glb"
```

You can also drag/drop `.obj`, `.gltf`, or `.glb` into the running window.

## Recent Commit Trail

- `731ac7a` Refactor renderer into explicit passes
- `a171256` Add HDR tonemapping pass
- `0539fd0` Add first point light shadow maps
- `6466765` Add directional shadow map pass
- `4542f19` Add tangent-space normal mapping
- `43e070d` Add static glTF import path
- `65186b7` Add PBR texture material sampling
- `eefa4cb` Add light viewport and shader outlines

## Recommended Next Steps

The user wants production-grade graphics capability over time. The next steps should build the base PBR pipeline in a way that can grow toward AAA-quality rendering.

Recommended order:

1. Multi-light data path.
   - Replace first-light-only uniforms with a small fixed-size GPU light list.
   - Support multiple directional, point, and spot lights.
   - Add spot light shading.
   - Keep this simple first, then later move to clustered/forward-plus.
2. Render target/pass infrastructure round two.
   - Turn named pass methods into pass descriptors with inputs/outputs.
   - Add named render targets: scene color, scene depth, shadow maps, post-process targets.
   - This prepares bloom, SSAO, IBL, deferred buffers, and shadow atlases.
3. Finish glTF material basics.
   - Emissive factor/texture.
   - Occlusion texture.
   - Alpha mode basics.
   - Explicit sRGB/linear texture handling.
4. IBL/environment lighting.
   - Skybox/environment cubemap.
   - Diffuse irradiance.
   - Prefiltered specular approximation.
   - BRDF LUT later.
5. Shadow system upgrade.
   - Per-light shadow settings.
   - Spot light shadow maps.
   - Better bias/normal-bias controls.
   - Directional cascades later.
   - Shadow atlas later.
6. Bloom and post-processing.
   - HDR threshold/extract pass.
   - Downsample/blur/composite chain.
   - Exposure/post settings.
   - Later color grading/LUT, vignette, SSAO/GTAO.
7. Stylized ZZZ-adjacent layer.
   - Toon ramps over PBR values.
   - Colored shadows.
   - Stylized specular/rim lighting.
   - Screen-space outlines.
   - Bloom/emissive accents.
   - Color grading presets.

## Suggested Immediate Next Task

Implement the multi-light data path.

Practical scope:

- Add a `GpuLight` or `ShaderLight` struct.
- Collect up to a small fixed maximum, such as 8 or 16 lights.
- Upload uniform arrays for kind, position, direction, color, intensity, range, spot angle.
- Update the PBR shader to loop over active lights.
- Preserve the existing first directional and first point shadow maps as the only shadowed lights for now.
- Add spot light contribution, even if spot shadows come later.
- Update docs and build.

This makes the renderer behave more like an actual engine while staying small enough to debug.
