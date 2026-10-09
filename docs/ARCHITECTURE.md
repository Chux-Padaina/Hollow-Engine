# Hollow Engine — Architecture

How Hollow Engine v0.4 is put together. For the feature list see the [README](../README.md).

## Layers

```
            ┌────────────────────────── Editor ──────────────────────────┐
            │ App (main loop, input → editor actions, undo, files, Play) │
            │ EditorUi (ImGui panels, emits EditorCommands)              │
            └──────────────┬──────────────────────────────┬──────────────┘
                           │ owns                         │ owns
            ┌──────────────▼──────────────┐   ┌───────────▼───────────┐
            │ Engine                      │   │ Renderer (OpenGL)     │
            │  Scene, AssetManager,       │   │  reads Scene + Camera │
            │  PhysicsWorld, ScriptSystem │   │  + RenderSettings     │
            │  RenderPipelineSettings     │   └───────────────────────┘
            └──────────────┬──────────────┘
                           │ uses
     Core (Math, Json, Log, FileUtil, Input, Properties) · Assets (MeshAsset, Importer, Animation, loaders) · Platform (Win32Window)
```

`Engine` is the runtime: the scene plus the systems that act on it. It has no editor or GL code, so a standalone game executable could drive it the same way `App` does. `App` is the editor shell around it, and `Renderer` draws whatever scene it's handed.

## Build modules

| Module | Contents | Links |
|---|---|---|
| `HollowRuntime.dll` | Core, Assets, Scene, Physics, Scripting (+ the plain-data `Camera` and `RenderSettings` stored in scenes) | Jolt (static), WIC |
| `HollowEngine.exe` | Editor, Platform, the renderer, `Main.cpp` | HollowRuntime, glad, ImGui, OpenGL |
| `Game.dll` | `engine/game/`: gameplay scripts | HollowRuntime |
| `HollowTests.exe` | `engine/tests/` | HollowRuntime |

The runtime is a shared library so the editor, the game module and the tests all use **one** copy of the engine's state: the log, the script registry, Jolt's allocator and type registry. Every function is exported (`WINDOWS_EXPORT_ALL_SYMBOLS`); public headers therefore avoid mutable globals and static data members (a data symbol can't be exported that way). Everything uses the shared CRT, so objects can be created in one module and freed in another.

## Modules

| Module | Files | Responsibility |
|---|---|---|
| Core | `Math`, `Json`, `Log`, `FileUtil`, `Input`, `Properties`, `PropertiesJson`, `Engine` | Vectors/matrices/quaternions/AABB/rays/frustum; JSON parse + write; log ring buffer; UTF-8 path helpers; input state + action bindings; property reflection (`PropertyVisitor`) and its JSON reader/writer; the Engine runtime object. |
| Platform | `Win32Window` | Window, message pump → `Input`, WGL 4.5 core context (via glad), drag-and-drop, native file dialogs. |
| Assets | `MeshAsset`, `MeshSimplify`, `AssetManager`, `Importer`, `Animation`, `TextureLoader`, `HdrLoader` | Immutable shared meshes with LODs and optional skins; quadric LOD generation; model cache; glTF/GLB/OBJ import (materials, skins, clips); skeletons, clip sampling and blending; WIC image decode; Radiance HDR decode. |
| Scene | `Scene`, `Components`, `SceneSerializer`, `SceneFactory`, `UndoStack`, `AnimationSystem` | Actors, hierarchy and components (with `describe()`); `.hscene` v2 + v1 migration + snapshots; spawn helpers and starter scenes; snapshot undo; animator update. |
| Physics | `PhysicsWorld` | Jolt-backed rigid bodies, colliders, terrain height fields, character controllers, contact/trigger events, layers, raycasts and overlap queries. |
| Scripting | `Script`, `ScriptSystem`, `BuiltinScripts`, `GameModule` | Script API + registry; Play-mode driver; example scripts; Game.dll loading and hot reload. |
| Render | `Camera`, `RenderSettings`, `Renderer` (+ private `Gfx`, `RenderGraph`, `ShaderLibrary`, `TextureManager`, `TextureCodec`, `GpuTypes`, `RendererImpl` and the `Renderer*.cpp` stages) | View camera; pipeline settings + per-frame view options; the GL 4.5 pipeline. |
| Editor | `App`, `EditorUi`, `EditorTheme`, `PropertyInspector` | Editor shell; UI panels; theme, icons and custom widgets; the inspector `PropertyVisitor`. |

Public headers live in `engine/include/Hollow/<Module>/`, implementations in `engine/src/<Module>/`. Headers private to a module (the renderer's GL wrappers, graph and GPU struct layouts) live next to their sources in `src/`. Shaders live in `engine/shaders/`.

## Main loop (`App::run`)

1. Pump window messages (fills `Input`; `WM_CLOSE` → "save changes?" prompt).
2. Start the ImGui frame; handle shortcuts (unless a text field has focus).
3. Open dropped files; viewport mouse: camera orbit/pan/zoom, gizmo/actor picking, drags.
4. Capture mode: turntable + skybox playlist; bake at most one playlist sky per frame.
5. `GameModuleHost::poll` — reloads `Game.dll` if it was rebuilt (deferred while playing).
6. `Engine::update(dt)` — in Play mode: scripts (`on_start`/`on_update`), then physics on a fixed 60 Hz accumulator (max 8 steps/frame), with contacts dispatched to scripts, then animators. Outside Play mode only animators with *Preview in editor* advance. Always refreshes world transforms.
7. In Play mode, if a primary `CameraComponent` exists, the view camera follows it (game view, editor overlays hidden).
8. `Renderer::render(scene, camera, settings, view, dt)` into an offscreen texture that the viewport panel shows (or `present()` when the UI is hidden).
9. `EditorUi::draw` → a list of `EditorCommand`s, executed by `App`.
10. Render ImGui, swap buffers, update the title.

## Scene graph

`Scene` owns actors (`std::vector<std::unique_ptr<Actor>>`, so references stay valid as actors are added). Each `Actor` has a stable `ActorId`, a name, an `active` flag, a local `Transform`, a parent id and child ids, and a list of components.

- **Transforms.** `Transform` is translation + XYZ Euler rotation (radians) + scale. `Scene::update_world_transforms()` walks the hierarchy once per frame and caches each actor's world matrix (`Actor::world_matrix()`); `Scene::compute_world_matrix(id)` computes one on demand (picking, gizmos).
- **Reparenting** (`set_parent`) refuses cycles and by default preserves the world transform by decomposing `inverse(parentWorld) * world`.
- **Components** are plain data deriving from `Component` (via a CRTP base that provides `type()` and `clone()`): `StaticMeshComponent` (mesh parts, each a `MeshHandle` + `Material`), `LightComponent`, `CameraComponent`, `RigidBodyComponent`, `ColliderComponent`, `ScriptComponent`, `ReflectionProbeComponent`, `IrradianceVolumeComponent`, `DecalComponent`, `ParticleEmitterComponent`, `TerrainComponent`, `WaterComponent`, `AnimatorComponent`, `CharacterControllerComponent`. Lookup is by `ComponentType` (no `dynamic_cast`). Script components may be stacked; other types are one per actor.
- **Reflection.** Each component overrides `describe(PropertyVisitor&)` and lists its fields with a key, a label, a range and a tooltip. `JsonWriteVisitor`/`JsonReadVisitor` turn that into the scene-file JSON; `InspectorVisitor` turns it into the inspector UI (with undo bracketing). Adding a field is one line. Components also carry a `revision` counter the renderer uses to notice edits (e.g. to re-bake a probe).
- **Lights** use physical units: lux for directional lights, lumens for point and spot lights (converted to candela as lm / 4π), with an optional color temperature, a range, a source size (soft shadows) and a *volumetric* flag.
- **Inactive actors** hide their whole subtree from rendering; physics and scripts skip inactive actors.
- **Conventions.** Actors look down local −Z (cameras, `forward_from_rotation`); lights shine along local −Y.

## Assets

A `MeshAsset` (vertices, indices, local AABB, a `source` string, a process-unique `id`) is immutable once created and shared through `MeshHandle = shared_ptr<const MeshAsset>`.

- `AssetManager::load_model(path)` imports a file once (cache keyed by the normalized, case-folded absolute path) into a `ModelAsset`: one `MeshAsset` per glTF primitive / OBJ material group, plus each part's imported `Material`. Importing the same file again, undoing, or loading a scene that references it reuses the same meshes.
- Built-in cube/sphere/plane come from `AssetManager::builtin_mesh`.
- `source` is how meshes are saved: `builtin:cube` or `model:<path>#<part>`. If a model can't be found on load, a visible placeholder cube keeps the original source so re-saving doesn't lose the reference.
- Import keeps the file's units and pivots (modular kits line up). Only files that are absurdly large or small (> 100 or < 0.02 units across) get a uniform scale on the *actor*, logged to the console.
- `make_mesh_asset` generates up to three LODs (50 / 25 / 12.5 % of the triangles, quadric error simplification that keeps UV seams and borders) for static meshes with at least 600 triangles, appended to the same index list.
- glTF: static meshes are flattened with node transforms baked into vertices (normals by the inverse-transpose, winding flipped for mirrored nodes). Files with skins or animations also get a `Skeleton` made from every node (parents first, rest pose = node TRS) holding the file's clips; skinned primitives keep bind-space vertices plus `JOINTS_0`/`WEIGHTS_0` influences and inverse bind matrices, and `spawn_model` adds an `AnimatorComponent`. Materials read the core PBR set plus `KHR_materials_emissive_strength`, `ior`, `specular`, `clearcoat`, `sheen`, `transmission`, `volume`, `anisotropy`, `diffuse_transmission`, `unlit` and `KHR_texture_transform` (base color). Embedded images are extracted to `%TEMP%\HollowEngine\<name>_<hash>_<n>.<ext>` (hash of the full path, size and mtime, so different models never collide). OBJ: vertices de-duplicated per material group.

## Scene files and undo

`SceneSerializer` turns a `Scene` into JSON and back.

```json
{
  "format": "hollow-scene", "version": 2,
  "environment": "../skybox/default_sky.hdr",
  "camera": { "target": [0, 0.5, 0], "yaw": 0.72, "pitch": 0.36, "distance": 7.5, "fov_degrees": 60.2 },
  "render_settings": { "shadows": { "filter": "Pcss", "...": "..." }, "exposure": { "mode": "Automatic", "...": "..." },
                       "sky": { "mode": "Atmosphere" }, "fog": { "...": "..." }, "post": { "tonemapper": "AgX", "...": "..." } },
  "physics": { "gravity": [0, -9.81, 0], "layers": { "layer0": "Default", "layer0_mask": 255, "...": "..." } },
  "scene": {
    "selected": 2,
    "actors": [
      { "id": 2, "name": "Player", "parent": 0, "active": true,
        "transform": { "position": [0, 1, 3], "rotation_degrees": [0, 0, 0], "scale": [1, 1, 1] },
        "components": [
          { "type": "StaticMesh", "parts": [ { "name": "Player", "mesh": { "builtin": "sphere" }, "material": { "base_color": [0.25, 0.55, 0.95], "...": "..." } } ] },
          { "type": "RigidBody", "mass": 1, "use_gravity": true, "...": "..." },
          { "type": "Collider", "shape": "sphere", "radius": 0.5, "...": "..." },
          { "type": "Script", "script": "PlayerController", "params": { "speed": 4, "jump_speed": 5.5 } }
        ] }
    ]
  }
}
```

Version 1 files (v0.2) are migrated on load: light intensities are rescaled into lux/lumens (×9830.4, point lights additionally by the old falloff), the old exposure multiplier becomes a manual EV100 (`13 − log2(exposure)`), and flat render settings map onto the nested structs.

Files store asset paths relative to the scene file when they're nearby (≤ 3 `..` levels on the same drive), so a project folder can move. Writes go through a temp file + rename. Loading parses into a fresh `Scene` and only replaces the live one on success.

**Undo/redo** (`UndoStack`) stores whole-scene snapshots (the same JSON, with absolute paths and no vertex data — meshes are references). Before an edit `App` records a snapshot: gizmo drags snapshot at mouse-down and commit at release only if something changed; ImGui widgets are bracketed with `IsItemActivated` / `IsItemDeactivatedAfterEdit` so one slider drag is one undo step; structural commands snapshot immediately. Whole-scene snapshots are simpler and much harder to get wrong than per-command inverse operations, and cheap at this engine's scene sizes.

**Play mode** uses the same mechanism: `App` snapshots the scene on Play and restores it on Stop. Undo is disabled while playing.

## Input

`Win32Window` translates `WM_KEYDOWN/UP` (and the `SYS` variants, so `Alt`/`F10` work), mouse and wheel messages into `Input`. `WM_KILLFOCUS` releases everything, so a key held while alt-tabbing doesn't stick. Each frame `begin_frame()` clears the pressed/released edges. Modifiers count if they were pressed this frame, so very fast chords still register. Gameplay uses named bindings (`bind_action`, `bind_axis`, `set_default_bindings`).

## Scripting

A script derives from `Script` and overrides `on_start`, `on_update(dt)`, `on_collision(other)` (every step while touching), `on_collision_enter/exit` and `on_trigger_enter/exit`. `ScriptRegistry` maps names to `ScriptInfo { description, parameter defaults, factory, module }`; a `ScriptComponent` stores the script name and its parameter values (numbers or text), and gets its runtime instance from `ScriptSystem` when Play starts. `ScriptContext` exposes the scene, the actor, the component's parameters, input, physics and the play time. Scripts must destroy actors through `ctx.destroy(id)` (deferred to the end of the update) so a script never deletes the object that is running it.

**Game modules.** Gameplay code lives in `engine/game/` and builds into `Game.dll`, which exports two functions through `HOLLOW_GAME_MODULE(registry) { ... }`: an API version and a registration function. `GameModuleHost` (owned by `Engine`):

1. copies `Game.dll` (and its PDB) into a fresh folder under `%TEMP%/HollowEngine/GameModules/` and loads the copy, so the compiler can overwrite the original and a debugger never locks the build's PDB (the module links with `/PDBALTPATH:Game.pdb`);
2. checks the API version, then calls the registration function with the registry tagged as module `"game"`;
3. every 0.5 s checks the original's timestamp; once it's been stable for 0.5 s (the linker finished) and the editor isn't playing, it removes the module's scripts, frees the old copy and loads the new one. While playing it waits and says so in the console.

Script *instances* only exist in Play mode, so a reload has no live objects to migrate: scene components keep their script names and parameters (new parameters are filled in with defaults).

## Physics

`PhysicsWorld` wraps [Jolt Physics](https://github.com/jrouwe/JoltPhysics) behind a pimpl; no Jolt header is visible outside `PhysicsWorld.cpp`. The scene stays the source of truth; each `step(scene, dt)` (fixed 1/60 s) does:

1. **Scene → Jolt.** For each active actor: a `CharacterControllerComponent` becomes a `JPH::CharacterVirtual`; a `ColliderComponent` becomes a body (static without a `RigidBodyComponent`, dynamic or kinematic with one, a kinematic sensor if it's a trigger); a `TerrainComponent` without a collider becomes a static 256×256 height field sampled from the terrain function. A body is rebuilt when its shape, material, layer or scale changes (hashed, plus the component's `revision`). Kinematic bodies follow their actor (`MoveKinematic`); a dynamic body moved by a script or the editor is teleported; velocities, angular velocities and pending impulses written by scripts are pushed in. Bodies whose actor disappeared are removed.
2. **Characters** move with `ExtendedUpdate` (stairs, floor sticking, sliding, max slope), with gravity applied while airborne; they push dynamic bodies, and their inner body lets triggers and raycasts see them.
3. **Simulate** with Jolt's job system (one thread per core, up to 8).
4. **Jolt → scene.** Active dynamic bodies write their position and rotation into the actor's local transform (through the parent's world matrix), plus velocities.
5. **Contacts.** A contact listener counts touching sub-shape pairs per body pair. Jolt also drops contacts when bodies fall asleep, so a pair whose count reaches zero is only reported as ended (exit event) if one of its bodies is still awake; sleeping pairs are kept (resting bodies stay `grounded`). Triggers report enter/exit for moving bodies only. Character contacts come from `CharacterVirtual::GetActiveContacts`.

Shapes: box (rounded by a small convex radius), sphere, capsule (local Y), mesh (the actor's triangles for static/kinematic bodies, their convex hull for dynamic ones), all scaled by the actor's world scale and offset by the collider's center. Layers: object layer = user layer × 2 + moving bit; static bodies never test against static bodies; `PhysicsSettings::layer_mask` decides which user layers collide (edited in *Render → Physics*, saved with the scene together with gravity).

## Renderer

The public `Renderer` is a thin pimpl; everything lives in `Renderer::Impl` (`src/Render/RendererImpl.hpp`), split by stage into `Renderer.cpp` (frame orchestration), `RendererScene.cpp` (extraction), `RendererGeometry.cpp` (culling, Hi-Z, batch drawing), `RendererShadows.cpp`, `RendererLighting.cpp`, `RendererEnvironment.cpp` (sky, HDRI, IBL), `RendererBake.cpp` (probe/volume baking), `RendererEffects.cpp` (skinning, grass, particles, transparency), `RendererPost.cpp` and `RendererOverlay.cpp`.

### GL foundations

- **Context.** `Win32Window` creates a WGL 4.5 core context (debug context in debug builds, with `KHR_debug` output routed to the log). All GL objects are created with DSA (`glCreate*`, `glNamed*`).
- **Reversed-Z.** `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`, a 32-bit float depth buffer cleared to 0 with `GL_GREATER`, and an infinite far plane for the camera. Shadow and capture projections are reversed too; CPU frustum culling keeps a conventional matrix.
- **Wrappers** (`Gfx`): RAII `Texture`/`Buffer`, a cached sampler set (including shadow compare samplers), a framebuffer cache, a GPU timestamp profiler and a fullscreen-triangle helper.
- **Shaders** (`ShaderLibrary`): files under `engine/shaders/` with `#include` and `#pragma once`. A program is a (vertex, fragment or compute, defines) permutation compiled on first use; errors are logged with file/line annotations. Every 0.5 s the library checks file timestamps and recompiles changed programs, keeping the old one on failure.
- **Textures** (`TextureManager`, `TextureCodec`): images are decoded on a worker thread (WIC), mipmapped on the CPU and compressed to BC1/BC3 (color), BC4 (single channel) or BC5 (normals, Z reconstructed in the shader), cached under `%LOCALAPPDATA%/HollowEngine/TextureCache`. DDS and KTX2 (with BCn data) load directly. Until a texture is ready the default texture is bound.
- **Bindings** (`GpuTypes.hpp` ↔ `shaders/bindings.glsl`): fixed UBO, SSBO and texture units shared by every shader. The frame UBO holds the camera, exposure, sun, sky SH, fog and feature bits; `scene_data.glsl` mirrors the instance / material / mesh / light / probe / decal structs.

### Render graph

Each frame `build_graph()` declares passes with `graph->add_pass<Data>(name, setup, execute)`. In `setup` a pass creates transient textures/buffers and declares reads and writes (sampled, image, render target, depth, storage, indirect...). `compile()` walks backwards from passes with side effects or writes to imported resources, culls the rest, and computes resource lifetimes; `execute()` takes transient textures from a pool (so non-overlapping resources alias the same GL texture), inserts `glMemoryBarrier`s for image/storage hazards, and times every pass with GPU queries (shown in *Stats*). Persistent resources (history buffers, shadow maps, probe arrays) are imported.

### Frame

`render()` runs: setup (jitter, matrices) → exposure readback → **scene extraction** → shadow setup → feature bits → environment update → buffer uploads → frame UBO → skinning palette → **graph** → bakes.

**Extraction** (`RendererScene.cpp`) turns the scene into GPU data. Every mesh lives in one shared vertex + index pool (`MeshPool`, a range allocator; meshes unused for ~300 frames are freed). Each visible mesh part becomes an `Instance` (model + previous model matrix, normal matrix, bounds, material, mesh slot, flags) sorted into **batches** by category (opaque, masked, refractive, transparent), cull mode and texture set. Materials are deduplicated into an SSBO. Terrains generate chunk meshes (33×33 vertices, stride LODs) and grass instances; water becomes a grid instance with its parameters packed into the material. Lights, probes, volumes and decals are packed into their own SSBOs.

**Passes** (in order; optional ones only when enabled):

| Pass | Notes |
|---|---|
| Skinning | Compute: posed vertices into per-instance pool regions (ping-pong, so the previous pose gives motion vectors). |
| Shadows | Per cascade / atlas tile: GPU cull → depth-only indirect draws. Sun: 4 cascades in a D32F array (texel snapped, depth clamped, blended at the seams). Spot/point: tiles from a buddy-allocated 4096² atlas sized by screen coverage. |
| Cull (phase 1) | Compute: instances visible last frame, frustum + LOD selection → indirect commands. |
| Grass cull | Compute: distance/density/frustum → visible blade list + `DrawArraysIndirect` args. |
| Depth prepass | Depth, octahedral normals + roughness, velocity. Opaque + masked batches and grass. |
| Hi-Z → cull (phase 2) → prepass (phase 2) | Min-depth pyramid; everything else tested against it; newly visible instances drawn. The union of both phases is used by later passes. |
| Light clusters | Compute: 16 tiles across the wider screen axis × 24 exponential slices; per cluster a list of lights, decals and probes. |
| Contact shadows, GTAO + blur, SSR + resolve | Screen-space inputs for the forward pass (SSR traces the previous frame's lit scene). |
| Volumetric fog | Froxels (8 px × 8 px × 64 slices): inject density/lighting with temporal reprojection, then integrate front to back. |
| Forward opaque | `GL_EQUAL` against the prepass depth; clustered lights, shadows (PCF or PCSS), decals, IBL (probes, volumes, sky), all material lobes, fog. |
| Sky | Physical atmosphere (sky-view LUT), HDRI or color where depth is still clear. |
| Copy opaque scene → refractive → transparent → particles | Mip-chained color + depth copy; water and glass refract it; WBOIT (accumulate + revealage, then composite) or sorted blending; particles simulate in compute and draw as soft billboards. |
| Copy scene color | History for next frame's SSR. |
| Post | Exposure histogram → TAA / TAAU → CAS → DOF → motion blur → bloom → tonemap + grade (3D LUT) + vignette / CA / grain → FXAA. |
| Editor overlay | Selection mask + outline, grid, icons, colliders, probe/volume boxes, gizmo. |

**GPU-driven drawing.** Commands are laid out as *union | phase 1 | phase 2 | one block per shadow view*, one `DrawElementsIndirect` per instance. A per-instance attribute (divisor 1, offset by `baseInstance`) gives the vertex shader its instance index, so each batch is one `glMultiDrawElementsIndirect`. *Performance → GPU culling* off switches to CPU frustum culling with one draw per visible instance. The visible-instance count shown in the viewport is read back two frames late, so it never stalls.

**Lighting model.** Lights are physical (lux, candela), the camera has an EV100 exposure (manual or histogram-based automatic with adaptation), and the forward pass writes pre-exposed radiance. Emissive is exposure-relative by default (so glTF emissive factors look the same at any exposure) or in nits. BRDF: GGX with height-correlated Smith visibility and multiscatter energy compensation, Burley diffuse, Charlie sheen, a Kelemen clearcoat lobe, anisotropic GGX, a wrapped-diffuse subsurface approximation, specular AA from normal variance.

**Environment.** The global environment is layer 0 of a cubemap array (128 px, GGX-prefiltered mips) plus L2 SH for diffuse. In *Atmosphere* mode transmittance/multiscatter LUTs are rebuilt when the sky parameters change, a sky-view LUT every frame, and layer 0 is re-captured when the sun moves (rate-limited). *HDRI* mode converts the equirect `.hdr` to a cubemap once (cached, up to 8). Reflection probes take the other layers; irradiance volumes store L1 SH in a 3D atlas (one z-slab per volume).

**Baking** (`RendererBake.cpp`). Probes and volumes re-bake when they are added, moved or edited, or on *Render → Bake lighting*. Work is time-sliced after the frame: one probe (6 faces × 128 px, then GGX filtering) or six volume probes (6 faces × 32 px each, projected to SH on the GPU) per frame. Captures use the forward shader's `PASS_CAPTURE` variant (every light, no screen-space effects, cascades chosen by world position) and the global environment as background. Volumes bake two bounces by default: bounce 1 is captured with the volume lit by bounce 0.

### Settings

`RenderPipelineSettings` (saved with the scene) groups shadows, exposure, sky, fog, GI, anti-aliasing, post, transparency and performance settings, each with a `describe()` that drives both the scene file and the *Render* tab. `RenderView` carries per-frame editor state (debug view, selection, gizmo, overlays).

## Editor

`EditorUi` builds the ImGui layout every frame (panel rectangles are computed from user-resizable sizes; splitters in the gaps between panels resize them, and the sizes and open states persist in `%LOCALAPPDATA%/HollowEngine/editor_layout.json`): a top bar (menus as popups, Play/Pause/Step/Stop, undo/redo/save, frame and GPU time), the outliner, the viewport (the renderer's output texture with floating tool and view toolbars and a status line), the inspector (*Details* / *Render* / *Stats* tabs), the console and the showcase window. `EditorTheme` holds the palette (near-black and grey surfaces, white text, neon lime #D4FF3F accents), the fonts (dynamic-size Segoe UI, regular + semibold), vector-drawn icons and the custom widgets: pill buttons, toggles, chips, segmented controls, filled-track sliders and animated collapsible section cards. Everything scales with the window height. Inspector fields come from each component's `describe()` through `PropertyInspector`, which brackets continuous edits into single undo steps. Gizmos are a constant size on screen. Their picking and dragging map mouse pixels to world units through the camera, so an axis drag tracks the cursor exactly. Clicking picks the nearest triangle under the cursor (bounding box test, then per-triangle).

## Tests

`engine/tests/EngineTests.cpp` builds into `HollowTests` (`ctest`, or `build.bat Release test`). It covers math (inverse/decompose/frustum/rays), JSON (escapes, round trip), hierarchy (keep-world reparenting, cycles, duplicate/delete), serialization round trips (including render settings), v1 → v2 migration, component property round trips, terrain basins, animation sampling and the animator system, LOD generation, undo, physics (resting contact height, a 3-box stack, a tipping box that settles flat, trigger enter/exit, collision layers, a walking character controller, overlap queries, terrain height fields, raycasts), a scripted Play-mode run of the demo scene, and importing the bundled models. It links only `HollowRuntime`, so it needs no window or GPU.

Shaders are validated offline with `glslangValidator` during development (every permutation the renderer compiles).

## Design rules

- **Ids, not pointers,** across frames (`ActorId`, `MeshAsset::id`).
- **Data in components, behaviour in systems.**
- **Fail loudly in the console, keep running.** Import, texture, environment, scene and shader failures are logged with the reason; the editor never silently swallows an error.
- **One description per struct.** Serialization and UI both go through `describe()`, so they can't drift apart.
- **Keep `GpuTypes.hpp` and `shaders/*.glsl` in sync** (bindings, struct layouts, flag bits); both files say so at the top.
