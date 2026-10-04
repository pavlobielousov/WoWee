// The Vita's shader manifest (VITA-53, VITA-14): every program the renderer can ask for, listed up front so the
// first-run compile has a known total and a determinate progress bar.
//
// TODAY the list holds only developer stand-ins: the GLSL ES 1.00 sources GlProbe measured on the device (terrain, M2,
// skinned character, water, sky, WMO, a stress shader), copied from tools/vita/glprobe/main.cpp. They exist to exercise
// and time the compile loop with realistic programs, and are used only when WOWEE_SHADER_SELFTEST=1 is set in env.txt.
// VITA-14 replaces them with the renderer's real shaders and makes the manifest unconditional.
#include "rendering/gl/shader_manifest.hpp"

#include "rendering/gl/shader_sources.hpp"

#include <cstdlib>

namespace wowee::rendering::gl {

namespace {

const char* const kTerrainVS =
    "precision highp float;\n"
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "uniform vec3 uLightDir;\n"
    "varying vec2 vUV;\n"
    "varying float vLight;\n"
    "varying float vFog;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "    vUV = aUV;\n"
    "    vLight = max(dot(vec3(0.0, 0.0, 1.0), uLightDir), 0.2);\n"
    "    vFog = clamp(gl_Position.w / 500.0, 0.0, 1.0);\n"
    "}\n";
const char* const kTerrainFS =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "varying float vLight;\n"
    "varying float vFog;\n"
    "uniform sampler2D uBase;\n"
    "uniform sampler2D uLayer1;\n"
    "uniform sampler2D uAlpha1;\n"
    "uniform vec3 uFogColor;\n"
    "void main() {\n"
    "    vec4 base = texture2D(uBase, vUV * 8.0);\n"
    "    vec4 l1 = texture2D(uLayer1, vUV * 8.0);\n"
    "    float a = texture2D(uAlpha1, vUV).r;\n"
    "    vec3 c = mix(base.rgb, l1.rgb, a) * vLight;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";
const char* const kM2VS =
    "precision highp float;\n"
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "varying vec2 vUV;\n"
    "void main() { gl_Position = uMVP * vec4(aPos, 1.0); vUV = aUV; }\n";
const char* const kM2FS =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uColor;\n"
    "void main() { vec4 t = texture2D(uTex, vUV); if (t.a < 0.5) discard; gl_FragColor = t * uColor; }\n";

const char* const kCharVS =
    "precision highp float;\n"
    "attribute vec3 aPos; attribute vec3 aNormal; attribute vec2 aUV; attribute vec4 aBones; attribute vec4 aWeights;\n"
    "uniform mat4 uViewProj; uniform mat4 uModel; uniform vec4 uBones[96];\n"
    "varying vec2 vUV; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "mat4 bone(float i) {\n"
    "    int k = int(i) * 3;\n"
    "    return mat4(vec4(uBones[k].x, uBones[k+1].x, uBones[k+2].x, 0.0), vec4(uBones[k].y, uBones[k+1].y, uBones[k+2].y, 0.0),\n"
    "                vec4(uBones[k].z, uBones[k+1].z, uBones[k+2].z, 0.0), vec4(uBones[k].w, uBones[k+1].w, uBones[k+2].w, 1.0));\n"
    "}\n"
    "void main() {\n"
    "    mat4 skin = bone(aBones.x) * aWeights.x + bone(aBones.y) * aWeights.y + bone(aBones.z) * aWeights.z + bone(aBones.w) * aWeights.w;\n"
    "    vec4 p = uModel * (skin * vec4(aPos, 1.0));\n"
    "    vNormal = normalize(mat3(uModel) * (mat3(skin) * aNormal));\n"
    "    vWorld = p.xyz; vUV = aUV;\n"
    "    gl_Position = uViewProj * p;\n"
    "    vFog = clamp(gl_Position.w / 400.0, 0.0, 1.0);\n"
    "}\n";
const char* const kCharFS =
    "precision mediump float;\n"
    "varying vec2 vUV; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "uniform sampler2D uTex; uniform sampler2D uEnv;\n"
    "uniform vec4 uLightPos[4]; uniform vec4 uLightColor[4]; uniform vec3 uEye; uniform vec3 uAmbient; uniform vec3 uFogColor;\n"
    "void main() {\n"
    "    vec4 base = texture2D(uTex, vUV);\n"
    "    if (base.a < 0.5) discard;\n"
    "    vec3 n = normalize(vNormal); vec3 v = normalize(uEye - vWorld);\n"
    "    vec3 lit = uAmbient;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        vec3 l = uLightPos[i].xyz - vWorld; float d = length(l); l /= d;\n"
    "        float att = clamp(1.0 - d / uLightPos[i].w, 0.0, 1.0);\n"
    "        float diff = max(dot(n, l), 0.0);\n"
    "        vec3 h = normalize(l + v);\n"
    "        float spec = pow(max(dot(n, h), 0.0), 24.0);\n"
    "        lit += uLightColor[i].rgb * (diff + 0.3 * spec) * att;\n"
    "    }\n"
    "    vec3 env = texture2D(uEnv, n.xy * 0.5 + 0.5).rgb;\n"
    "    vec3 c = base.rgb * lit + env * 0.15;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, base.a);\n"
    "}\n";
const char* const kWaterVS =
    "precision highp float;\n"
    "attribute vec3 aPos; attribute vec2 aUV;\n"
    "uniform mat4 uViewProj; uniform float uTime; uniform vec3 uEye;\n"
    "varying vec2 vUV; varying vec2 vUV2; varying vec3 vView; varying float vFog;\n"
    "void main() {\n"
    "    vec3 p = aPos;\n"
    "    p.z += 0.15 * sin(p.x * 0.7 + uTime) + 0.1 * sin(p.y * 1.1 + uTime * 1.3) + 0.05 * sin((p.x + p.y) * 2.3 + uTime * 2.1);\n"
    "    vUV = aUV * 6.0 + vec2(uTime * 0.03, 0.0); vUV2 = aUV * 11.0 - vec2(0.0, uTime * 0.05);\n"
    "    vView = uEye - p;\n"
    "    gl_Position = uViewProj * vec4(p, 1.0);\n"
    "    vFog = clamp(gl_Position.w / 600.0, 0.0, 1.0);\n"
    "}\n";
const char* const kWaterFS =
    "precision mediump float;\n"
    "varying vec2 vUV; varying vec2 vUV2; varying vec3 vView; varying float vFog;\n"
    "uniform sampler2D uWater; uniform sampler2D uNormal; uniform vec3 uDeep; uniform vec3 uShallow; uniform vec3 uSun; uniform vec3 uFogColor;\n"
    "void main() {\n"
    "    vec3 nm = texture2D(uNormal, vUV).rgb * 2.0 - 1.0;\n"
    "    vec3 nm2 = texture2D(uNormal, vUV2).rgb * 2.0 - 1.0;\n"
    "    vec3 n = normalize(vec3((nm.xy + nm2.xy) * 0.6, 1.0));\n"
    "    vec3 v = normalize(vView);\n"
    "    float fres = pow(1.0 - max(dot(n, v), 0.0), 4.0);\n"
    "    vec3 tex = texture2D(uWater, vUV + n.xy * 0.05).rgb;\n"
    "    vec3 body = mix(uShallow, uDeep, fres) * tex;\n"
    "    vec3 h = normalize(uSun + v);\n"
    "    float spec = pow(max(dot(n, h), 0.0), 64.0);\n"
    "    vec3 c = body + uSun * spec * 0.8 + fres * 0.25;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, 0.55 + fres * 0.35);\n"
    "}\n";
const char* const kSkyVS =
    "precision highp float;\n"
    "attribute vec3 aPos; uniform mat4 uViewProj; varying vec3 vDir;\n"
    "void main() { vDir = aPos; gl_Position = (uViewProj * vec4(aPos, 1.0)).xyww; }\n";
const char* const kSkyFS =
    "precision mediump float;\n"
    "varying vec3 vDir; uniform vec3 uZenith; uniform vec3 uHorizon; uniform vec3 uSunDir; uniform vec3 uSunColor;\n"
    "void main() {\n"
    "    vec3 d = normalize(vDir);\n"
    "    float t = clamp(d.z, 0.0, 1.0);\n"
    "    vec3 sky = mix(uHorizon, uZenith, pow(t, 0.6));\n"
    "    float sun = max(dot(d, normalize(uSunDir)), 0.0);\n"
    "    sky += uSunColor * (pow(sun, 256.0) * 2.0 + pow(sun, 8.0) * 0.25);\n"
    "    gl_FragColor = vec4(sky, 1.0);\n"
    "}\n";
const char* const kWmoVS =
    "precision highp float;\n"
    "attribute vec3 aPos; attribute vec3 aNormal; attribute vec2 aUV; attribute vec4 aColor;\n"
    "uniform mat4 uViewProj; uniform mat4 uModel;\n"
    "varying vec2 vUV; varying vec4 vColor; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "void main() {\n"
    "    vec4 p = uModel * vec4(aPos, 1.0);\n"
    "    vWorld = p.xyz; vNormal = normalize(mat3(uModel) * aNormal); vUV = aUV; vColor = aColor;\n"
    "    gl_Position = uViewProj * p;\n"
    "    vFog = clamp(gl_Position.w / 500.0, 0.0, 1.0);\n"
    "}\n";
const char* const kWmoFS =
    "precision mediump float;\n"
    "varying vec2 vUV; varying vec4 vColor; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "uniform sampler2D uTex; uniform sampler2D uLightmap; uniform vec4 uLightPos[4]; uniform vec4 uLightColor[4];\n"
    "uniform vec3 uSunDir; uniform vec3 uSunColor; uniform vec3 uAmbient; uniform vec3 uFogColor; uniform float uAlphaRef;\n"
    "void main() {\n"
    "    vec4 base = texture2D(uTex, vUV);\n"
    "    if (base.a < uAlphaRef) discard;\n"
    "    vec3 n = normalize(vNormal);\n"
    "    vec3 lit = uAmbient * vColor.rgb + uSunColor * max(dot(n, normalize(uSunDir)), 0.0);\n"
    "    lit += texture2D(uLightmap, vUV).rgb * 0.5;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        vec3 l = uLightPos[i].xyz - vWorld; float d = length(l);\n"
    "        float att = clamp(1.0 - d / uLightPos[i].w, 0.0, 1.0);\n"
    "        lit += uLightColor[i].rgb * max(dot(n, l / d), 0.0) * att * att;\n"
    "    }\n"
    "    vec3 c = base.rgb * lit;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, base.a * vColor.a);\n"
    "}\n";
const char* const kStressFS =
    "precision mediump float;\n"
    "varying vec2 vUV; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "uniform sampler2D uT0; uniform sampler2D uT1; uniform sampler2D uT2; uniform sampler2D uT3; uniform sampler2D uT4; uniform sampler2D uT5;\n"
    "uniform vec4 uLightPos[8]; uniform vec4 uLightColor[8]; uniform vec3 uEye; uniform vec3 uFogColor;\n"
    "void main() {\n"
    "    vec4 a = texture2D(uT0, vUV) * texture2D(uT1, vUV * 2.0);\n"
    "    vec4 b = mix(texture2D(uT2, vUV * 4.0), texture2D(uT3, vUV * 4.0), texture2D(uT4, vUV).r);\n"
    "    vec3 n = normalize(vNormal + (texture2D(uT5, vUV * 3.0).rgb - 0.5) * 0.6);\n"
    "    vec3 v = normalize(uEye - vWorld);\n"
    "    vec3 lit = vec3(0.1);\n"
    "    for (int i = 0; i < 8; i++) {\n"
    "        vec3 l = uLightPos[i].xyz - vWorld; float d = length(l); l /= d;\n"
    "        float att = clamp(1.0 - d / uLightPos[i].w, 0.0, 1.0);\n"
    "        vec3 h = normalize(l + v);\n"
    "        lit += uLightColor[i].rgb * (max(dot(n, l), 0.0) + 0.5 * pow(max(dot(n, h), 0.0), 32.0)) * att;\n"
    "    }\n"
    "    vec3 c = mix(a.rgb, b.rgb, 0.5) * lit;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

}  // namespace

const std::vector<ProgramDef>& shaderManifest() {
    // Developer stand-ins (WOWEE_SHADER_SELFTEST=1): realistic programs for timing the first-run build.
    static const std::vector<ProgramDef> standIns = {
        {"stand-in terrain (2 layers)", ProgramClass::Plain, kTerrainVS, kTerrainFS},
        {"stand-in M2 static (alpha test)", ProgramClass::Plain, kM2VS, kM2FS},
        {"stand-in sky dome", ProgramClass::Plain, kSkyVS, kSkyFS},
        {"stand-in WMO group", ProgramClass::Lit, kWmoVS, kWmoFS},
        {"stand-in water", ProgramClass::Lit, kWaterVS, kWaterFS},
        {"stand-in skinned character", ProgramClass::Skinned, kCharVS, kCharFS},
        {"stand-in stress (8 lights)", ProgramClass::Lit, kWmoVS, kStressFS},
    };
    // The renderer's own programs: EVERY program the renderer can build must be listed here, or the stale-file sweep
    // deletes its cache entry at the next start (DEV_SETUP section 21).
    static const std::vector<ProgramDef> all = [] {
        std::vector<ProgramDef> list;
        for (int layers = 0; layers <= 3; ++layers) list.push_back(terrainProgram(layers));
        list.push_back(m2Program(M2Kind::Opaque));
        list.push_back(m2Program(M2Kind::AlphaTest));
        list.push_back(m2Program(M2Kind::Blend));
        return list;
    }();
    static const std::vector<ProgramDef> withStandIns = [] {
        std::vector<ProgramDef> list = all;
        list.insert(list.end(), standIns.begin(), standIns.end());
        return list;
    }();
    const char* flag = std::getenv("WOWEE_SHADER_SELFTEST");
    return (flag && *flag == '1') ? withStandIns : all;
}

}  // namespace wowee::rendering::gl
