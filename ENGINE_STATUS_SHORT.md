# Forge Engine Short Status

## Goal

Build a native C++/OpenGL engine-editor demo with a production-shaped graphics pipeline. The long-term visual target is a stylized anime/urban renderer inspired by the broad ingredients of Zenless Zone Zero: crisp silhouettes, expressive PBR materials, colored lighting, emissive accents, bloom/post effects, and eventually toon/stylized controls.

No proprietary ZZZ assets, UI, logos, models, environments, or exact content should be copied.

## Active Code

- Use the C++ project in `cpp/`.
- The Rust project in `src/` is legacy reference.

Build:

```powershell
.\build_cpp.bat
```

Run:

```powershell
.\cpp\build-msvc\ForgeEngine.exe
```

## Current Progress

- Win32 OpenGL window.
- Dear ImGui editor UI.
- Actor/component scene model.
- Outliner, details panel, primitive/light creation, deletion.
- Editor camera controls, actor selection, move/rotate/scale gizmos.
- OBJ/glTF/GLB static mesh import.
- Base color, metallic/roughness, and normal texture pipeline.
- Direct-light Cook-Torrance/GGX PBR shader.
- Directional and point light contribution.
- First directional shadow map.
- First point light shadow maps.
- HDR scene framebuffer.
- ACES-style tonemapping with exposure control.
- Explicit renderer pass order through `RenderFrameContext` and `RenderPassKind`.

## Next Recommended Step

Implement the multi-light data path:

- collect multiple scene lights
- upload fixed-size shader light arrays
- add spot light shading
- keep current first directional/point shadows as the only shadowed lights for now

After that, continue toward IBL/environment lighting, emissive/AO material support, bloom, and a more formal render-target/pass system.
