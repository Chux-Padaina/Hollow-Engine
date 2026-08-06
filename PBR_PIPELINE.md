# PBR Render Pipeline Notes

This document tracks the steps needed to turn the current lighting/debug renderer into a basic physically based rendering pipeline. It is meant to be both a build checklist and a learning reference for why each part exists.

## Goal

Build a practical glTF/Sketchfab-friendly PBR renderer first, then stylize it later. The final direction is still a ZZZ-adjacent portfolio engine, but the foundation should understand standard PBR assets before toon ramps, colored shadows, bloom, and outlines are layered back in.

## Current Baseline

Implemented in the Rust reference:

- Mesh import for `.glb`, `.gltf`, and `.obj`.
- glTF hierarchy transform preservation.
- Base color factors, vertex colors, UVs, and base color textures.
- Scene lights with directional, point, and retained spot data.
- Direct-light Cook-Torrance/GGX PBR shader.
- Per-mesh metallic and roughness factors.
- glTF metallic and roughness factors.
- Details-panel material sliders for selected-group metallic and roughness.
- Lit floor cube, editor grid, simple planar projected shadows.
- Albedo, normals, lighting-only, roughness, and metallic debug views.

Pending in the new C++ implementation:

- Complete modern OpenGL renderer for all scene/editor passes.
- glTF import.
- Texture-backed material inspector and debug views.

Recently added in the new C++ implementation:

- Dear ImGui editor UI for actor transforms, light controls, and basic material factors.
- OBJ import with material diffuse color preservation.
- First shader/VBO scene mesh path, with immediate-mode fallback if required GL entry points are unavailable.
- Persistent GPU cache for imported/custom mesh parts.
- First C++ Cook-Torrance/GGX object shader with directional and point light contribution.
- C++ albedo, normal, lighting-only, roughness, and metallic debug views for scene meshes.
- Temporary light gray editor background before the future skybox/environment lighting pass.
- First C++ shader-based mesh actor outline pass.
- Texture-slot storage on C++ materials.
- Windows WIC texture loading for common image formats.
- OBJ `.mtl` texture path import.
- Base-color and combined metallic/roughness texture sampling in the C++ PBR shader.
- Static `.gltf`/`.glb` mesh and material import in C++.
- Tangent generation and tangent-space normal map sampling in the C++ PBR shader.
- First directional-light shadow map pass in C++.
- First point-light shadow map pass in C++ for the first enabled point light.

Not yet implemented in either path:

- Occlusion and emissive texture import.
- Cascaded or production-quality shadow maps.
- Multi-point/spot light shadow maps and shadow atlasing.
- Image-based lighting/reflection probes.
- HDR framebuffer/post-processing.

## Proposed Pipeline Steps

### 1. Direct-Light PBR Shader

Implement a metallic-roughness Cook-Torrance shader for direct lights.

Inputs:

- base color
- metallic
- roughness
- normal from mesh vertex normals
- directional light
- point light

BRDF pieces:

- GGX normal distribution
- Smith-Schlick geometry term
- Schlick Fresnel
- Lambert diffuse for non-metals
- energy split between diffuse and specular

Why this matters:

This is the core material model used by modern real-time renderers. It lets roughness and metallic values mean something predictable under lights.

Status: implemented for direct directional and point lighting in the active C++ renderer.

Implementation notes:

- The shader uses metallic-roughness Cook-Torrance lighting for directional and point lights.
- The BRDF uses GGX distribution, Smith-Schlick visibility, and Schlick Fresnel.
- Meshes now carry `MaterialProperties` with `metallic` and `roughness`.
- glTF `metallicFactor` and `roughnessFactor`, OBJ material factors, and editor factor controls are active.
- The default test scene uses a rough non-metal cube, a smoother metal cube, and a rough non-metal floor.
- This is not full engine PBR yet because it does not include image-based lighting, HDR, or post-processing.

### 2. Material Properties And Inspector Controls

Add per-material or per-object controls for:

- base color factor
- metallic factor
- roughness factor
- emissive factor
- normal strength

Why this matters:

Before importing all texture maps, sliders let us validate the math with simple cubes and known values.

Status: partially implemented and shader-connected for scalar/factor controls plus stored texture slots.

Implementation notes:

- The C++ Dear ImGui details panel exposes base color, metallic, and roughness controls for selected mesh actors.
- Normal strength is exposed for selected mesh actors and is connected to tangent-space normal map sampling.
- The details panel also shows imported texture slot paths for base color, metallic/roughness, normal, and emissive maps when present.
- The current control applies the same factor values to every mesh in the selected group. That keeps testing simple, but later we should expose per-material slots so imported multi-material assets can preserve different settings per part.
- Emissive factor controls are still pending.

### 3. PBR Debug Views

Add debug views for:

- roughness
- metallic

Why this matters:

When a model looks wrong, these views answer a basic question quickly: did the material values make it into the shader, or is the lighting model the problem?

Status: implemented for active C++ scene meshes using scalar/factor material values and sampled metallic/roughness textures.

Implementation notes:

- `F5` displays roughness as grayscale.
- `F6` displays metallic as grayscale.
- These views show the final scalar material factor multiplied by the sampled metallic/roughness texture channel when present.

### 4. Render Pass Hygiene And Stylized Outlines

Move the renderer closer to the final pass structure:

- editor/world background
- non-scene editor grid
- actor outline pass
- opaque PBR actor pass
- editor icons and gizmos

Why this matters:

The final ZZZ-adjacent style depends on layered rendering. Clean pass order prevents the grid, icons, outlines, and lit geometry from fighting each other.

Status: partially implemented.

Implementation notes:

- The active C++ renderer now has a shader-based expanded-backface outline pass for mesh actors.
- Planes are skipped by the outline pass so floor/editor planes do not get thick border halos.
- The current outline width is object-space adjusted by object scale. A later pass should make it screen-pixel-stable, so tiny and huge imported models feel consistent.

### 5. glTF Metallic-Roughness Texture Import

Read glTF material properties:

- `baseColorFactor`
- `metallicFactor`
- `roughnessFactor`
- `baseColorTexture`
- `metallicRoughnessTexture`

In glTF, metallic and roughness usually share one texture:

- green channel: roughness
- blue channel: metallic

Why this matters:

This is the standard Sketchfab/glTF workflow. Without it, imported assets cannot preserve their intended material response.

Status: implemented for the static mesh renderer/import path.

Implementation notes:

- The C++ material struct now has texture slots for base color, combined metallic/roughness, normal, and emissive maps.
- The C++ renderer can load PNG/JPG-style image files through Windows Imaging Component and upload them as OpenGL textures.
- The object shader samples base-color textures and combined metallic/roughness textures.
- OBJ `.mtl` import records common texture map references.
- glTF/GLB import now reads static triangle meshes, node transforms, UVs, vertex colors, base color factors, metallic/roughness factors, and texture references.
- This is still not a complete glTF renderer path because skins, animation, morph targets, camera import, and light extensions are pending.

### 6. Normal Mapping And Tangents

Add tangent-space normal mapping.

Needed data:

- generated tangents from positions and UVs
- normal map texture
- tangent-bitangent-normal matrix in shader

Why this matters:

Normal maps carry most of the small surface detail in modern assets: tire tread, fabric weave, panel grooves, screws, vents, bevels, and scratches.

Status: implemented for the current static mesh renderer/import path.

Implementation notes:

- Imported OBJ and glTF mesh parts generate tangents from triangle positions and UVs.
- Built-in cube, plane, and sphere meshes carry tangent data.
- The C++ shader samples normal maps, reconstructs the world-space shading normal through TBN, and uses that normal for PBR lighting and the normal debug view.
- glTF `normalTexture.scale` is imported as material normal strength.
- Authored glTF tangent attributes are not preserved yet; generation is good enough for most static test assets, but authored tangents may be useful later for perfect parity with DCC exports.

### 7. Emissive And Ambient Occlusion

Read and use:

- emissive factor
- emissive texture
- occlusion texture

Why this matters:

Emissive surfaces will be important for signs, screens, decals, and ZZZ-style accent lighting. Ambient occlusion helps contact areas and creases retain depth before full global illumination exists.

Status: pending.

### 8. Real Directional Shadow Map

Replace planar blob shadows with a basic shadow map.

Required passes:

- render scene depth from directional light view
- sample shadow map in PBR shader
- apply depth bias
- add simple PCF filtering

Why this matters:

Real shadows prove that objects can cast onto other objects, not just the floor. This is the first real engine-style shadow system.

Status: implemented as a first directional-light shadow map.

Implementation notes:

- The renderer creates a 2048x2048 depth texture and framebuffer for the first enabled directional light.
- Before the main viewport pass, the scene renders through a depth-only shader from the light's orthographic camera.
- The PBR shader samples the shadow map and attenuates direct directional light.
- Shadows can be toggled from the editor toolbar.
- The current light-space volume is centered around the editor camera target. This is good for a prototype/editor test scene, but not final game camera coverage.
- Filtering is a simple 3x3 PCF kernel. Bias and softness will need visual tuning.
- Not implemented yet: cascaded shadow maps, per-light shadow settings, production point/spot light shadows, contact shadows, or transparent-shadow handling.

### 9. Point-Light Shadowing

Add omnidirectional point-light shadowing so local lights are blocked by nearby geometry.

Why this matters:

Point lights are common in game scenes: bulbs, signs, lamps, vehicle lights, muzzle flashes, and glowing props. If they do not cast shadows, they brighten objects through walls or through other meshes, which makes the lighting feel fake even before stylization.

Status: first-pass implemented for the first enabled point light in the active C++ renderer.

Implementation notes:

- The renderer allocates six depth textures for the first enabled point light, one for each major direction around the light.
- Each frame, the scene is rendered into those six depth maps from the point light position with 90-degree perspective views.
- The PBR shader picks the correct face from the fragment's direction away from the point light, samples that depth map, and attenuates only the point-light contribution when blocked.
- This is intentionally not the final production system yet. Later work should add smoother cross-face filtering, better bias controls, multiple shadowed point lights, spot light shadows, shadow atlases, and eventually clustered/forward-plus light selection.

### 10. HDR Framebuffer And Tonemapping

Render the scene into an HDR color buffer instead of directly to the window.

Add:

- HDR framebuffer
- exposure control
- ACES or Reinhard tonemapping
- gamma correction

Why this matters:

PBR lighting often exceeds `0..1`. HDR lets bright lights, emissive materials, and future bloom behave correctly.

Status: pending.

### 11. Basic Environment Lighting

Start simple, then improve.

First version:

- ambient irradiance color
- roughness-aware fake reflection term

Later version:

- cubemap sky/environment
- diffuse irradiance cubemap
- prefiltered specular cubemap
- BRDF lookup texture

Why this matters:

Metals and glossy surfaces look wrong without environment reflections. Direct lights alone are not enough for convincing PBR.

Status: pending.

### 12. Full PBR Debug Views

Add debug views for:

- base color
- normals
- roughness
- metallic
- occlusion
- emissive
- direct lighting
- shadows
- final shaded

Why this matters:

Debug views make asset import problems obvious. They also make this project better as a learning/portfolio piece.

Status: partially implemented.

Implementation notes:

- Base color, normals, lighting-only, roughness, metallic, and final shaded views are implemented.
- Occlusion, emissive, shadow, and texture-specific debug views are still pending.

### 13. Stylized PBR Pass

After the basic PBR renderer is stable, add stylization.

Possible techniques:

- quantized diffuse bands
- colored shadow ramps
- stylized rim lighting
- graphic/specular shaping
- outlines
- emissive bloom accents
- color grading
- halftone or subtle screen texture

Why this matters:

This is where the renderer moves from "standard PBR" toward the final ZZZ-adjacent identity, while still preserving imported material data.

Status: pending.

## Recommended Immediate Next Steps

1. Visual QA the C++ PBR shader and debug views in the app.
2. Add glTF import with metallic/roughness factors and textures.
3. Add tangent support and normal maps.
4. Replace planar shadows with a directional shadow map.
5. Add basic environment lighting so metals have something to reflect.
6. Move grid, gizmos, and light icons out of immediate mode.

The current C++ material-control milestone should make it easy to demonstrate:

- plastic: metallic `0`, roughness medium/high
- shiny plastic: metallic `0`, roughness low
- rough metal: metallic `1`, roughness high
- polished metal: metallic `1`, roughness low
