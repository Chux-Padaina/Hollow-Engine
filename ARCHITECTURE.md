# Hollow Engine — Architecture

This is a detailed tour of how Hollow Engine v0.1 is put together. For a quick feature list see [README.md](README.md); for what's planned next see [ROADMAP.md](ROADMAP.md).

## Module map

All engine code lives under `cpp/`, split into `include/Hollow/*.hpp` (public interfaces) and `src/*.cpp` (implementations), one pair per module. Every file starts with a short header comment describing its own responsibility — this section is the same information gathered in one place.

| Module | Responsibility |
|---|---|
| `Main.cpp` | Executable entry point: forwards command-line arguments as startup imports to `App`. |
| `App` | Top-level owner and main-loop driver. Creates the window/renderer/scene, ticks `Engine` each frame, turns raw input into editor actions (camera orbit/pan, gizmo dragging, ray-based picking, drag-and-drop import), and loads the bundled default environment at startup. |
| `Win32Window` | Win32 window creation, the message pump, and the WGL OpenGL context. GL function pointers are loaded by hand — there's no GLFW/SDL dependency. |
| `Engine` | Shared editor/render state: the active tool (select/move/rotate/scale), transform space, style preset, debug view, and `RenderPipelineSettings` (every renderer toggle/slider the UI binds to). |
| `Scene` | The Actor/Component scene graph, mesh/material/vertex types, and `make_default_scene()`. |
| `Renderer` | The OpenGL PBR render pipeline: shadow maps, SSAO, IBL, the main object pass, actor outlines/grid/gizmos, and HDR post-processing. The largest module in the engine. |
| `Importer` | Loads glTF/GLB and OBJ files from disk into `MeshPart` data that `Scene::create_mesh_actor` consumes. |
| `Json` | A minimal hand-rolled JSON parser used only to read `.gltf` files. |
| `Math` | `Vec2`/`Vec3`, column-major `Mat4`, and a TRS `Transform` — no engine dependencies, shared by `Renderer`, `Scene`, and `Importer`. |
| `TextureLoader` | Decodes PNG/JPEG (or anything WIC supports) into an RGBA8 buffer for GPU upload. |
| `HdrLoader` | Decodes Radiance `.hdr` (RGBE) environment maps for IBL. |
| `EditorUi` | The Dear ImGui editor chrome: toolbar, outliner, details panel, debug windows, theme, and hand-drawn toolbar icons. |

## Runtime shape

`App::run` owns the whole process: it creates the Win32 window and OpenGL context, initializes `Renderer` and `EditorUi`, loads the bundled default environment map (see "Default scene and environment" below), then imports any command-line startup paths and enters the main loop.

Each frame: poll Win32 messages → read keyboard/mouse and translate it into editor actions (`App::handle_keyboard`/`handle_mouse`) → import any files dropped since last frame → `Engine::tick` (currently just style/debug-view/tool bookkeeping) → `Renderer::resize` to the current client size → `Renderer::render` → draw the editor UI → swap buffers → update the window title (actor count, selection, and a control hint string).

Camera control is a simple orbit camera (`Camera::target`/`distance`/`pitch`/`yaw`), not a free-fly camera — right-drag orbits, middle-drag pans the target, the wheel zooms.

### Object picking

`App::pick_actor` ray-casts from the camera through the clicked pixel and tests against each actor's world-space bounding sphere, taking the nearest hit along the ray — not the actor whose 2D screen-space projection happens to be closest to the click. `App::make_pick_ray` builds that ray directly from the camera's own basis vectors and vertical FOV rather than unprojecting through an inverse view-projection matrix; `Math.hpp` deliberately has no `Mat4::inverse()`, so every place that would normally need one (this, the cascade shadow frustum fit, SSAO's world-position reconstruction) instead rebuilds what it needs from the camera's basis and projection parameters directly. That's valid because the camera is always a symmetric perspective projection.

The bounding sphere radius comes from `MeshPart::bounds_radius` (computed at import time as the farthest vertex distance from the model's re-centered origin); built-in primitives fall back to a fixed radius, and light actors get a small fixed-radius sphere matching their icon size. A sphere, not a per-triangle test, is a deliberate scoping choice — it can over-select a few pixels past an object's actual silhouette (e.g. a flat plane's corners), which is a smaller usability problem than requiring a click near an object's exact center.

### Default scene and environment

`Scene::make_default_scene()` builds a single sphere and one directional light — deliberately minimal, so a fresh checkout shows one clean, readable object rather than a cluttered test scene. The default environment map is handled separately, outside the scene graph: `App::run` resolves `assets/skybox/default_sky.hdr` relative to the running executable (`App`'s `executable_directory()` helper, via `GetModuleFileNameW`, not the process's current working directory — which varies by launch method and can't be relied on for a relative path) and loads it through `Renderer::load_environment` before the main loop starts. CMake copies `cpp/assets/` next to the built executable as a post-build step so this path resolves the same way whether the engine is launched from the VS/CLion debugger, a build output folder, or a double-click. Dropping a different `.hdr` file onto the window at any time replaces it.

## Scene graph

`Scene` owns a flat list of `Actor`s (no hierarchical parenting yet — every actor's transform is relative to world space). Each `Actor` has a root `Transform` and a small set of optional components: `StaticMeshComponent` (one or more `MeshPart`s, each with its own `Material`) or `LightComponent` (`LightKind::Directional`/`Point`/`Spot`, with color/intensity/range/spot-angle). `PlaceholderComponent` exists as a named stand-in for future component kinds (physics, audio, scripting, ...) that don't have real implementations yet.

`Material` carries the full metallic-roughness set: base color (+ alpha and alpha mode: opaque/mask/blend), metallic, roughness, normal strength, emissive factor, occlusion strength, and an optional texture path for each of base color / metallic-roughness / normal / emissive / occlusion.

## Renderer

`Renderer::render` executes a fixed, explicit pass order every frame (see `Renderer::describe_passes()`, which the editor's Render Graph debug window reads directly — the pass list below is not documentation drifting from the code, it's the same data):

| Pass | Reads | Writes |
|---|---|---|
| Shadow Maps | — | 4 CSM cascades, spot shadow map |
| Scene Color Setup | — | `SceneColor` |
| Ambient Occlusion | — | `SceneNormalDepth`, `SceneAORaw`, `SceneAO` |
| Actor Outlines | — | `SceneColor` |
| Editor Grid | — | `SceneColor` |
| Opaque Geometry | shadow maps, `SceneAO` | `SceneColor` |
| Transparent Geometry | shadow maps, `SceneAO` | `SceneColor` |
| Bloom | `SceneColor` | 5 downsample mips |
| Tonemap | `SceneColor`, bloom mip 0 | backbuffer (or an LDR target if FXAA is on) |
| FXAA | tonemapped LDR color | backbuffer |
| Editor Overlay | — | backbuffer |

A few ordering choices are load-bearing rather than incidental. Actor outlines draw before the editor grid: both write into the same depth buffer, and near an object's base the outline shell sits almost coplanar with the grid plane — with the grid drawn first it could win that depth-test tie and visibly cut through the outline right where an object meets the ground. Ambient occlusion runs before geometry because the opaque/transparent passes sample `SceneAO` as an input. Bloom, tonemapping, vignette, and color grading all happen in the HDR path and are skipped entirely when HDR is off; FXAA runs on the tonemapped LDR result, after tonemapping, not as an MSAA replacement earlier in the pipeline.

### Shading

The main object shader is a metallic-roughness Cook-Torrance/GGX BRDF, evaluated against up to 16 lights (directional/point/spot, mixed freely) per draw call, plus the baked IBL terms and the SSAO occlusion term. `Engine::debug_view()` can swap the shader's final output to any of 8 intermediate values (albedo, normals, lighting-only, roughness, metallic, occlusion, emissive) for debugging, and `Engine::style()` applies one of 3 preset material/lighting biases.

### Shadows

Directional light shadows use a 4-cascade CSM, with per-cascade view-projection matrices and split depths recomputed from the camera frustum each frame; the object shader selects a cascade per-fragment from view-space depth. Spot lights get a single shadow map. Point-light shadows exist as a six-face cube render (`render_point_shadow_maps`) but are the oldest/least maintained of the three and are called out as legacy in the render-graph target names — the first enabled directional/point/spot light effectively owns "the" shadow for now, which is a real limitation for scenes with several shadow-casting lights (see ROADMAP.md).

### Image-based lighting

`Renderer::load_environment` decodes a `.hdr` file (`HdrLoader`) and bakes it into an irradiance convolution (diffuse IBL) and a GGX-prefiltered specular mip chain, sampled in the object shader alongside the direct lights. The same environment drives the skybox draw. `RenderPipelineSettings::ibl_enabled` gates both; `sky_intensity` is a separate multiplier applied only to the skybox draw, independent of scene `exposure`, since most HDRI environments encode the sun/sky at a raw radiance that reads as overly bright and feeds too much energy into the bloom threshold otherwise.

### Ambient occlusion

A hemisphere-kernel SSAO pass reconstructs view-space position/normal from depth (again without a matrix inverse — see "Object picking" above for why that pattern recurs) and darkens the ambient/IBL term directly in the object shader, the same place the glTF occlusion texture is applied. It's independent of HDR, unlike bloom/vignette/grading.

### Post-processing

Bloom is a threshold-and-downsample chain (5 mips) with a soft knee at the threshold so bright areas fade into the effect instead of clipping on/off; the blurred result is added back in the tonemap pass. Tonemapping is ACES-style with an exposure control. Vignette and color grading (contrast/saturation/temperature) are both parametric — applied in LDR space after tonemapping/gamma, not via a texture-based 3D LUT. FXAA is a single-pass luma-edge-directed blend, not the full NVIDIA FXAA 3.11 reference algorithm (no per-edge length search or sub-pixel term). All of these are deliberate scope cuts, not oversights — see ROADMAP.md for what a fuller version of each would add.

### Actor outlines and gizmos

Selected actors get a shader-based outline: an expanded-backface shell rendered in a second pass rather than a post-process edge-detect, so the outline follows the mesh silhouette exactly. Move/rotate/scale gizmos are drawn as camera-facing 3D handles (arrows, rings, boxes) with hover/active highlighting, respecting the active local/world transform space toggle. The world grid is a camera-following visual guide only — it doesn't receive shadows, block movement, or constrain objects to a ground plane.

## Editor UI

`EditorUi` is built on Dear ImGui 1.93 (WIP) using only the classic `imgui_impl_opengl2` + `imgui_impl_win32` backends — the project does not have ImGui's docking branch wired in, so panels are fixed-position (toolbar top, outliner left, details right), not dockable/tabbable.

**Theme.** A deliberate gray-and-neon theme (`apply_style()`) rather than ImGui's defaults: mostly-grayscale panels with exactly two accent colors (a saturated blue for active/primary state, a brighter cyan-leaning blue for hover/secondary state), and sharply reduced corner rounding for an angular, "tech HUD" look. Section headers (`section_header()`) draw a small diagonal accent flag instead of ImGui's default separator-with-text. The four main tool buttons draw a small hand-drawn vector icon (built only from `ImDrawList::AddLine`/`AddTriangleFilled`, deliberately avoiding newer path-stroke APIs for version safety) next to their label.

**Scaling and resizing.** The whole UI scales with window size: a `ui_scale` factor derived from window height (1.0× at 720p, growing to 2.5× on very tall/4K windows) drives `io.FontGlobalScale`, `apply_style()`'s spacing/rounding parameters, and the hardcoded pixel constants in the toolbar and hand-drawn icons via a per-frame file-local `g_ui_scale`. The outliner and details panels are horizontally resizable by dragging their border, using ImGui's own native per-window resize grip (height is constrained to exactly the space below the toolbar; width is free within a min/max range) rather than a hand-rolled splitter.

**Panels.** The toolbar holds the tool buttons, transform-space toggle, snap/shadow/outline/HDR/IBL checkboxes, exposure/sky sliders, a resolution preset combo, and toggles for two debug windows. The outliner lists every actor with add/delete controls. The details panel shows the selected actor's transform and component-specific controls (light color/intensity/range/angle, or mesh part count and per-material PBR controls — material edits currently apply to every part of the selected actor's mesh group, not per-part). The Render Graph window is a read-only view onto `describe_passes()`/the resolved render-target registry; the Post FX window exposes every `RenderPipelineSettings` field grouped by effect.

## Build system

CMake (3.24+) builds two targets: a static `imgui` library from the vendored sources under `cpp/vendor/imgui` (classic backends only), and the `HollowEngine` executable linking against it plus `OpenGL::GL` and the Win32 libraries texture loading/drag-drop need (`shell32`, `windowscodecs`, `ole32`). A post-build step copies `cpp/assets/` next to the built executable so bundled runtime assets (currently just the default skybox) resolve at a stable path regardless of launch method.

## Design rules

A few patterns recur deliberately across the codebase, not by accident:

- **No `Mat4::inverse()`.** Every place that would normally reach for an inverse view-projection matrix (object picking, CSM frustum fitting, SSAO reconstruction) instead rebuilds what it needs directly from the camera's basis vectors and projection parameters, since the camera projection is always a symmetric perspective. Simpler and cheaper than a general 4×4 inverse, at the cost of not generalizing to an off-axis/asymmetric projection if one is ever needed.
- **Bounding spheres over precise geometry** for picking. Cheap, good enough, and the failure mode (over-selecting a few pixels past an object's silhouette) is minor.
- **Parametric over texture-based effects** where AAA engines typically use a texture (color grading uses contrast/saturation/temperature sliders, not a 3D LUT). Simpler to implement and tune live; a LUT would allow more expressive, non-parametric grades.
- **Raw GLSL string literals stay under MSVC's ~16,380-character-per-literal limit.** `Renderer.cpp`'s shader strings are checked against this after any edit — the largest is currently under 9,000 characters.
