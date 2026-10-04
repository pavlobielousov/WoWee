# Vita shaders (VITA-14)

Hand-written GLSL ES 1.00 (ADR-001 decision 5), embedded in `src/rendering/gl/shader_sources.cpp`, built once per console into
the shader cache (`DEV_SETUP.md` sections 21 and 22). Costs are per full-screen pass on a real Vita Slim, additively blended
(so the tile-based GPU cannot skip hidden surfaces), 960x544 / 640x368 in an FBO, compiler O2 unless noted.

| Upstream shader | Vita program | Status | Cost 960x544 / 640x368 |
|---|---|---|---|
| `terrain.vert/frag` | `terrain (base)`, `(1 layer)`, `(2 layers)`, `(3 layers)` | done, 20/20 pixel checks | 1.25 / 0.52, 1.93 / 0.90, 2.61 / 1.17, 3.08 / 1.46 ms |
| `m2.vert/frag` (static) | `M2 static (opaque)` | done | 1.38 / 0.75 ms |
| | `M2 static (alpha test)` (compiled at O3) | done | 2.16 / 1.01 ms (3.10 / 1.40 at O2) |
| | `M2 static (blend)` (alpha, and additive with `uParams.w = 0`) | done | 1.38-1.41 / 0.75 ms |
| `m2.vert` skinning, `character.*` | skinned M2 / character | VITA-20 (GPU vs CPU skinning; vertex uniform limit is 128 vec4) | stand-in compile 1.05 s |
| `wmo.*` | WMO | not yet | |
| `water.*` | water | not yet | |
| `skybox`, `celestial`, `starfield`, `clouds` | sky | not yet | |
| `m2_particle`, `m2_ribbon`, `m2_smoke` | particles | not yet | |
| `minimap`, `overlay`, UI | UI (ImGui's own GLES2 program) and minimap | UI program is ImGui's, cached | |
| compute (`m2_cull*`, `hiz_*`, `grass_cull`, `fsr2_*`), shadows, post-process, volumetric fog, sun shafts, RT, lens flare, grass | none | cut (ADR-001) | |

Interface notes (what the renderer sets):
- Terrain: samplers `uBase`, `uLayer1..3`, `uAlpha` (RGBA, the alpha maps of layers 1..3 in r, g, b, packed per chunk at upload: VITA-18);
  uniforms `uViewProj`, `uModel`, `uLightDir`, `uLightColor`, `uAmbient`, `uEye`, `uFog` (x start, y end), `uFogColor`;
  attributes `aPosition`, `aNormal`, `aTexCoord` (tiling), `aLayerUV` (0..1 per chunk).
- M2: sampler `uTexture`; uniforms as above plus `uUVOffset`, `uLit` (x lit 0/1, y emissive for unlit), `uTint`,
  `uParams` (x alpha cutoff, y colour-key luminance, z fade, w fog to colour 1 / to black 0). GL blend state is the caller's:
  opaque none; alpha test and blend `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`; additive `SRC_ALPHA, ONE`.
- Matrices for vitaGL must be built with `glm::perspectiveRH_NO` and a Z-up `lookAt`: the shared `rendering::Camera` is built
  for Vulkan (`GLM_FORCE_DEPTH_ZERO_TO_ONE` is global). A VITA-18 task.
- Compressed textures: **only with the patched vitaGL** (`tools/vita/build_vitagl.sh`).
