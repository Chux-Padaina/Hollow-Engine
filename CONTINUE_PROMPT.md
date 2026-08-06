# Fresh Thread Prompt

Use this prompt to continue the project in a new Codex thread:

```text
We are working in:
C:\Users\aadha\Documents\Tools\Game Engine

Project goal:
Build a portfolio-grade native C++/OpenGL game-engine/editor prototype focused on a production-quality graphics pipeline. The long-term visual target is a stylized anime/urban renderer inspired by the broad rendering ingredients of Zenless Zone Zero, but without copying proprietary ZZZ assets, UI, models, environments, logos, or exact content. The engine should import user-provided Sketchfab-style assets and eventually render them with crisp outlines, strong PBR material response, colored/stylized lighting, emissive accents, post-processing, and a polished editor viewport.

Important:
- Active implementation is C++ under cpp/.
- The old Rust prototype under src/ is reference only unless I explicitly ask to work on it.
- Use C++/OpenGL and keep the code readable for someone who knows C++.
- Use apply_patch for manual edits.
- Build with:
  .\build_cpp.bat
- Run with:
  .\cpp\build-msvc\ForgeEngine.exe
- Optional model path:
  .\cpp\build-msvc\ForgeEngine.exe "C:\path\to\model.glb"

Current state:
- Native Win32 OpenGL window.
- Dear ImGui editor UI.
- Actor/component scene model.
- Outliner, details panel, add/delete objects/lights.
- Static mesh and light components.
- Placeholder component/system slots for later engine features.
- Camera controls like an editor viewport.
- Select/move/rotate/scale tools with local/world toggle and grid snap.
- C++ import supports OBJ, glTF, and GLB by command line or drag/drop.
- glTF static mesh import preserves node transforms, normals, UVs, vertex colors, base color, metallic/roughness factors, normal strength, external/embedded buffers, and image references.
- Renderer has modern shader/VBO path for scene meshes.
- PBR shader supports base color, metallic/roughness, normal maps, directional light, point light, debug views.
- Directional shadow map exists for first enabled directional light.
- Point shadow maps exist for first enabled point light using six depth maps.
- HDR scene color framebuffer exists with ACES-style tonemapping and exposure control.
- Renderer was refactored into explicit pass order:
  1. shadow maps
  2. scene color target setup
  3. editor grid
  4. actor outlines
  5. opaque geometry and light icons
  6. tonemap
  7. editor overlay/gizmos

Useful docs:
- README.md: broad overview and build/run.
- ENGINE_STATUS_SHORT.md: short goal/progress readme.
- HANDOVER.md: detailed handoff.
- PBR_PIPELINE.md: PBR pipeline roadmap.
- ENGINE_ARCHITECTURE.md: engine/editor architecture notes.
- RUST_PARITY_AUDIT.md: Rust-to-C++ parity ledger.

Recent commit:
731ac7a Refactor renderer into explicit passes

Recommended next task:
Implement the multi-light data path for the base PBR pipeline.

Suggested scope:
- Add a small fixed-size shader light list, maybe 8 or 16 lights.
- Collect multiple directional, point, and spot lights from scene actors.
- Upload uniform arrays to the PBR shader.
- Update shader lighting loop so multiple lights contribute.
- Implement spot light contribution.
- Keep the existing first directional and first point shadow maps as the only shadowed lights for now.
- Update README/PBR_PIPELINE/HANDOVER as needed.
- Build with .\build_cpp.bat and commit.

Constraints/taste:
- Do not skip foundational architecture steps just for quick visuals.
- We want production-grade capability eventually, so prefer systems that can grow: render targets, pass descriptors, light data, material data, debug views.
- Keep visuals editor-friendly and readable while the pipeline is under construction.
```
