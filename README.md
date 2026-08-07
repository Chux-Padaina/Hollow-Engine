# Hollow Engine

**v0.1** — a native C++20/OpenGL game engine and level editor, built from scratch as a portfolio project.

Hollow Engine imports glTF/GLB/OBJ models and renders them with a real-time, metallic-roughness PBR pipeline: multi-light shading, cascaded + spot shadow maps, image-based lighting from an HDR environment, screen-space ambient occlusion, and an HDR post-processing stack (bloom, vignette, color grading, FXAA). Around that sits a Dear ImGui editor — an outliner, a details/inspector panel, transform gizmos, ray-accurate object picking, and a render-graph/post-FX debug view — styled with a custom gray-and-neon theme instead of ImGui's defaults.

It targets Windows: the windowing/input layer is a native Win32 + WGL implementation (no GLFW/SDL — GL function pointers are loaded by hand), and texture decoding goes through the Windows Imaging Component.

## Features

**Rendering**
- Metallic-roughness PBR object shader, up to 16 directional/point/spot lights per draw call
- Cascaded shadow maps (4 cascades) for the directional light, plus a spot-light shadow map
- Image-based lighting: irradiance + GGX specular convolution baked from a loaded `.hdr` environment, with a skybox draw
- Screen-space ambient occlusion (hemisphere-kernel, depth+normal based)
- HDR pipeline: ACES-style tonemapping, bloom, vignette, parametric color grading, FXAA
- Full glTF material support: base color, metallic/roughness, tangent-space normal mapping, emissive, occlusion, alpha blend/mask
- 8 debug views (`F1`–`F8`): final, albedo, normals, lighting-only, roughness, metallic, occlusion, emissive
- 3 style presets (`1`/`2`/`3`) for quick material/lighting look changes

**Editor**
- Outliner, details/inspector panel, and a toolbar with select/move/rotate/scale gizmo tools
- Ray-accurate click-anywhere-on-the-object picking (bounding-sphere ray cast, not a fixed on-screen click radius)
- Local/world transform space toggle, grid snapping
- Render Graph and Post FX debug windows (live pass reads/writes, resolved target list, all post-processing controls)
- Drag-and-drop import of `.obj`/`.gltf`/`.glb` models and `.hdr` environments
- UI scales with window size, and the outliner/details panels resize by dragging their border

## Building

Requires Visual Studio 2022 (Community or better) with the C++ build tools, and CMake 3.24+.

From PowerShell, in this folder:

```powershell
.\build_cpp.bat
```

This opens the VS developer environment, configures CMake (`cpp\build-msvc`, NMake Makefiles), and builds the engine. Then run it:

```powershell
.\cpp\build-msvc\HollowEngine.exe
```

You can also pass a model path on the command line to import it at startup:

```powershell
.\cpp\build-msvc\HollowEngine.exe "C:\path\to\model.glb"
```

A default scene (one sphere, one directional light, and a bundled default environment map) loads automatically — no arguments required.

## Controls

| Input | Action |
|---|---|
| `Q` / `W` / `E` / `R` | Select / Move / Rotate / Scale tool |
| `X` | Toggle local/world transform space |
| `G` | Toggle grid snapping |
| Click | Select the object under the cursor |
| Right mouse drag | Orbit camera |
| Middle mouse drag | Pan camera |
| Mouse wheel | Zoom camera |
| `4` / `5` / `6` | Add directional / point / spot light |
| `Delete` | Delete the selected actor |
| `Home` | Reset scene |
| `F1`–`F8` | Debug views (final, albedo, normals, lighting-only, roughness, metallic, occlusion, emissive) |
| `1` / `2` / `3` | Style presets |
| `Esc` | Quit |

## More

- [ARCHITECTURE.md](ARCHITECTURE.md) — a detailed tour of the current engine and editor architecture.
- [ROADMAP.md](ROADMAP.md) — what's built, what's in progress, and what's planned next.
