# Engine Architecture Notes

This document tracks the long-term engine/editor skeleton. The goal is not to clone Unreal Engine's full scale, but to keep the prototype shaped like a real expandable 3D engine while the current implementation stays small.

The active engine path is now C++/OpenGL under `cpp/`. The original Rust implementation remains a reference during migration.

## Reference Model

The current architecture follows the broad Unreal-style mental model:

- A level/world owns placeable actors.
- The outliner shows actors, not separate lists for meshes, lights, cameras, or scripts.
- Actors have a root scene component that owns transform data.
- Components provide behavior/data such as static mesh rendering, lights, script behavior, animation, audio, input, collision, or cameras.
- Engine systems consume components rather than hard-coding every feature into the actor type.

## Current Runtime Shape

Implemented in the active C++ skeleton:

- `App`
  - owns the native window and frame loop
  - owns `Renderer`
  - owns `EditorUi`
  - owns `Scene`
  - owns an `Engine` subsystem container
- `Scene`
  - owns `std::vector<std::unique_ptr<Actor>>`
  - tracks the selected actor id
- `Actor`
  - owns a name
  - owns a root `SceneComponent`
  - owns a component list
  - has placeholder hierarchy fields: parent and children
- `SceneComponent`
  - owns local transform
  - owns mobility
- Active components:
  - `StaticMeshComponent`
  - `LightComponent`
- Placeholder component types:
  - `SkeletalMeshComponent`
  - `CameraComponent`
  - `CollisionComponent`
  - `PhysicsBodyComponent`
  - `ScriptComponent`
  - `AudioComponent`
  - `AnimationComponent`
  - `InputComponent`
  - `UiComponent`
  - `ParticleSystemComponent`
  - `NavigationAgentComponent`
  - `AiControllerComponent`
  - `PostProcessVolumeComponent`
  - `NetworkReplicationComponent`
- Placeholder subsystem slots:
  - Asset Registry
  - Renderer
  - Editor Selection
  - Input
  - Physics
  - Audio
  - Animation
  - Scripting
  - UI
  - Post Processing
  - Save/Load

Implemented in the Rust reference but pending in C++:

- drag/drop glTF/OBJ import
- precise viewport picking
- polished interactive move/rotate/scale gizmos
- shader-based PBR rendering
- material debug views
- drag/drop file import and asset material preservation

## Current Editor Shape

Implemented now:

- One actor-centric outliner.
- Dear ImGui editor shell with toolbar, outliner, and details panels.
- Actor details panel:
  - actor transform first
  - then component-specific controls
- Static mesh actor controls:
  - mesh part count
  - mesh part names
  - metallic and roughness controls
- Light actor controls:
  - enabled
  - color
  - intensity
  - range for local lights
  - spot angle for spot lights
- Gizmo transform operations apply to the selected actor.

## Intended Growth Path

The next systems should attach to this skeleton instead of creating parallel scene lists.

Future components:

- Camera component
- Collision/physics component
- Skeletal mesh component
- Animation component with graph/clip state
- Script component
- Audio source component
- Post-process volume component
- Controller/input receiver component

Future systems:

- Asset registry and asset drawer
- Scene save/load and prefab/blueprint-like actor templates
- Input mapping for keyboard, mouse, and controller
- Script lifecycle: construct, begin play, tick, destroy
- Animation update and pose upload
- Audio mixer/source update
- Physics/collision update
- Render graph and post-processing pipeline
- Real lighting pipeline: shadow maps, image-based lighting, HDR, tonemapping

## Design Rules

- Put every placeable thing in the outliner as an actor.
- Put behavior/data on components whenever practical.
- Let systems query components from actors.
- Keep the grid as editor-only visual guidance.
- Do not clamp actor movement to the grid plane.
- Prefer one canonical scene model over parallel object/light/camera lists.
- Keep runtime/editor concepts separate where possible, but allow the current prototype to share code until modules are split further.
