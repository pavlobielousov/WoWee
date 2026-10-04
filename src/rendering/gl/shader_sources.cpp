// See shader_sources.hpp (VITA-14).
#include "rendering/gl/shader_sources.hpp"

#include <string>

namespace wowee::rendering::gl {

namespace {

// ---- terrain ------------------------------------------------------------------------------------------------------
// Vertex: lighting and fog per vertex. uLightDir is the direction the light travels (upstream's lightDir).
const char* const kTerrainVertex =
    "precision highp float;\n"
    "attribute vec3 aPosition;\n"
    "attribute vec3 aNormal;\n"
    "attribute vec2 aTexCoord;\n"
    "attribute vec2 aLayerUV;\n"
    "uniform mat4 uViewProj;\n"
    "uniform mat4 uModel;\n"
    "uniform vec4 uLightDir;\n"
    "uniform vec3 uLightColor;\n"
    "uniform vec3 uAmbient;\n"
    "uniform vec4 uEye;\n"
    "uniform vec4 uFog;\n"  // x = start, y = end
    "varying highp vec2 vTexCoord;\n"
    "varying highp vec2 vLayerUV;\n"
    "varying vec3 vLight;\n"
    "varying float vFog;\n"
    "void main() {\n"
    "    vec4 world = uModel * vec4(aPosition, 1.0);\n"
    "    float diff = max(dot(normalize(aNormal), normalize(-uLightDir.xyz)), 0.0);\n"
    "    vLight = uAmbient + diff * uLightColor;\n"
    "    vFog = clamp((uFog.y - distance(uEye.xyz, world.xyz)) / (uFog.y - uFog.x), 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "    vLayerUV = aLayerUV;\n"
    "    gl_Position = uViewProj * world;\n"
    "}\n";

const char* const kTerrainFragmentBody =
    "precision mediump float;\n"
    "varying highp vec2 vTexCoord;\n"
    "varying highp vec2 vLayerUV;\n"
    "varying vec3 vLight;\n"
    "varying float vFog;\n"
    "uniform sampler2D uBase;\n"
    "#if TERRAIN_LAYERS >= 1\n"
    "uniform sampler2D uAlpha;\n"
    "uniform sampler2D uLayer1;\n"
    "#endif\n"
    "#if TERRAIN_LAYERS >= 2\n"
    "uniform sampler2D uLayer2;\n"
    "#endif\n"
    "#if TERRAIN_LAYERS >= 3\n"
    "uniform sampler2D uLayer3;\n"
    "#endif\n"
    "uniform vec3 uFogColor;\n"
    "void main() {\n"
    "    vec4 color = texture2D(uBase, vTexCoord);\n"
    "#if TERRAIN_LAYERS >= 1\n"
    "    vec3 a = texture2D(uAlpha, vLayerUV).rgb;\n"
    "    color = mix(color, texture2D(uLayer1, vTexCoord), a.r);\n"
    "#endif\n"
    "#if TERRAIN_LAYERS >= 2\n"
    "    color = mix(color, texture2D(uLayer2, vTexCoord), a.g);\n"
    "#endif\n"
    "#if TERRAIN_LAYERS >= 3\n"
    "    color = mix(color, texture2D(uLayer3, vTexCoord), a.b);\n"
    "#endif\n"
    "    gl_FragColor = vec4(mix(uFogColor, color.rgb * vLight, vFog), 1.0);\n"
    "}\n";

// ---- static M2 ----------------------------------------------------------------------------------------------------
// uLit is 1 for a lit batch and 0 for an unlit one (which shows the texture times uEmissive), a multiplier and not a
// branch or another permutation. uUVOffset is the instance's texture scroll.
const char* const kM2Vertex =
    "precision highp float;\n"
    "attribute vec3 aPosition;\n"
    "attribute vec3 aNormal;\n"
    "attribute vec2 aTexCoord;\n"
    "uniform mat4 uViewProj;\n"
    "uniform mat4 uModel;\n"
    "uniform vec2 uUVOffset;\n"
    "uniform vec4 uLightDir;\n"
    "uniform vec3 uLightColor;\n"
    "uniform vec3 uAmbient;\n"
    "uniform vec4 uEye;\n"
    "uniform vec4 uFog;\n"
    "uniform vec2 uLit;\n"  // x = lit (0 or 1), y = emissive multiplier for an unlit batch
    "varying highp vec2 vTexCoord;\n"
    "varying vec3 vLight;\n"
    "varying float vFog;\n"
    "void main() {\n"
    "    vec4 world = uModel * vec4(aPosition, 1.0);\n"
    "    float diff = max(dot(normalize(mat3(uModel) * aNormal), normalize(-uLightDir.xyz)), 0.0);\n"
    "    vLight = mix(vec3(uLit.y), uAmbient + diff * uLightColor, uLit.x);\n"
    "    vFog = clamp((uFog.y - distance(uEye.xyz, world.xyz)) / (uFog.y - uFog.x), 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord + uUVOffset;\n"
    "    gl_Position = uViewProj * world;\n"
    "}\n";

const char* const kM2FragmentBody =
    "precision mediump float;\n"
    "varying highp vec2 vTexCoord;\n"
    "varying vec3 vLight;\n"
    "varying float vFog;\n"
    "uniform sampler2D uTexture;\n"
    "uniform vec3 uFogColor;\n"
    "uniform vec3 uTint;\n"
    "uniform vec4 uParams;\n"  // x = alpha cutoff, y = colour-key luminance (0 = off), z = fade, w = fog to colour (1) or black (0)
    "void main() {\n"
    "    vec4 tex = texture2D(uTexture, vTexCoord);\n"
    "#if M2_ALPHATEST\n"
    "    if (tex.a < uParams.x) discard;\n"
    "    if (dot(tex.rgb, vec3(0.299, 0.587, 0.114)) < uParams.y) discard;\n"
    "#endif\n"
    "    vec3 lit = tex.rgb * uTint * vLight;\n"
    "    vec3 fogged = mix(uFogColor * uParams.w, lit, vFog);\n"
    "#if M2_BLEND\n"
    "    gl_FragColor = vec4(fogged, tex.a * uParams.z);\n"
    "#else\n"
    "    gl_FragColor = vec4(fogged, 1.0);\n"
    "#endif\n"
    "}\n";


// ---- WMO groups ----------------------------------------------------------------------------------------------------
// Same fragment shader as the static M2 (texture times a per-vertex light, fog, alpha test or blend). The light follows
// upstream's wmo.frag in three modes picked by uMode (weights, so no branch): x = exterior (ambient + sun, plus half the
// baked vertex colour when the group has one, uMode.w), y = interior (the baked vertex colour, at least the WMO's
// ambient), z = unlit.
const char* const kWmoVertex =
    "precision highp float;\n"
    "attribute vec3 aPosition;\n"
    "attribute vec3 aNormal;\n"
    "attribute vec2 aTexCoord;\n"
    "attribute vec4 aColor;\n"
    "uniform mat4 uViewProj;\n"
    "uniform mat4 uModel;\n"
    "uniform vec4 uLightDir;\n"
    "uniform vec3 uLightColor;\n"
    "uniform vec3 uAmbient;\n"
    "uniform vec3 uWmoAmbient;\n"
    "uniform vec4 uEye;\n"
    "uniform vec4 uFog;\n"
    "uniform vec4 uMode;\n"
    "varying highp vec2 vTexCoord;\n"
    "varying vec3 vLight;\n"
    "varying float vFog;\n"
    "void main() {\n"
    "    vec4 world = uModel * vec4(aPosition, 1.0);\n"
    "    float diff = max(dot(normalize(mat3(uModel) * aNormal), normalize(-uLightDir.xyz)), 0.0);\n"
    "    vec3 ext = uAmbient + diff * uLightColor + uMode.w * aColor.rgb * 0.5;\n"
    "    vec3 inner = max(aColor.rgb, uWmoAmbient);\n"
    "    vLight = uMode.x * ext + uMode.y * inner + vec3(uMode.z);\n"
    "    vFog = clamp((uFog.y - distance(uEye.xyz, world.xyz)) / (uFog.y - uFog.x), 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "    gl_Position = uViewProj * world;\n"
    "}\n";

// ---- scene blit ---------------------------------------------------------------------------------------------------
// Scales the off-screen 3D scene up to the screen (the render scale, VITA-19): a quad in clip space, one texture fetch.
const char* const kBlitVertex =
    "precision highp float;\n"
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "varying highp vec2 vTexCoord;\n"
    "void main() {\n"
    "    vTexCoord = aTexCoord;\n"
    "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "}\n";

const char* const kBlitFragment =
    "precision mediump float;\n"
    "varying highp vec2 vTexCoord;\n"
    "uniform sampler2D uTexture;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(texture2D(uTexture, vTexCoord).rgb, 1.0);\n"
    "}\n";

struct Built {
    std::string vertex;
    std::string fragment;
};

Built* terrainSources() {
    static Built built[4];
    static bool done = false;
    if (!done) {
        for (int i = 0; i < 4; ++i) {
            built[i].vertex = kTerrainVertex;
            built[i].fragment = "#define TERRAIN_LAYERS " + std::to_string(i) + "\n" + kTerrainFragmentBody;
        }
        done = true;
    }
    return built;
}

Built* wmoSources() {
    static Built built[3];
    static bool done = false;
    if (!done) {
        const char* defs[3] = {"#define M2_ALPHATEST 0\n#define M2_BLEND 0\n", "#define M2_ALPHATEST 1\n#define M2_BLEND 0\n",
                               "#define M2_ALPHATEST 0\n#define M2_BLEND 1\n"};
        for (int i = 0; i < 3; ++i) {
            built[i].vertex = kWmoVertex;
            built[i].fragment = std::string(defs[i]) + kM2FragmentBody;
        }
        done = true;
    }
    return built;
}

Built* m2Sources() {
    static Built built[3];
    static bool done = false;
    if (!done) {
        const char* defs[3] = {"#define M2_ALPHATEST 0\n#define M2_BLEND 0\n", "#define M2_ALPHATEST 1\n#define M2_BLEND 0\n",
                               "#define M2_ALPHATEST 0\n#define M2_BLEND 1\n"};
        for (int i = 0; i < 3; ++i) {
            built[i].vertex = kM2Vertex;
            built[i].fragment = std::string(defs[i]) + kM2FragmentBody;
        }
        done = true;
    }
    return built;
}

}  // namespace

const ProgramDef& terrainProgram(int layers) {
    static const char* const names[4] = {"terrain (base)", "terrain (1 layer)", "terrain (2 layers)",
                                         "terrain (3 layers)"};
    static ProgramDef defs[4];
    static bool done = false;
    if (!done) {
        Built* b = terrainSources();
        for (int i = 0; i < 4; ++i) {
            defs[i] = ProgramDef{names[i], ProgramClass::Plain, b[i].vertex.c_str(), b[i].fragment.c_str(),
                                 {"aPosition", "aNormal", "aTexCoord", "aLayerUV"}};
        }
        done = true;
    }
    return defs[layers < 0 ? 0 : (layers > 3 ? 3 : layers)];
}

const ProgramDef& m2Program(M2Kind kind) {
    static const char* const names[3] = {"M2 static (opaque)", "M2 static (alpha test)", "M2 static (blend)"};
    static ProgramDef defs[3];
    static bool done = false;
    if (!done) {
        Built* b = m2Sources();
        for (int i = 0; i < 3; ++i) {
            // The alpha-test variant is compiled at SHARK_OPT_FAST (O3): measured on the device, `discard` shaders run about
            // 30 % faster with it (2.16 against 3.10 ms per full-screen pass), while every other program is the same speed
            // at O2 and compiles up to twice as fast.
            const int level = i == static_cast<int>(M2Kind::AlphaTest) ? 3 : -1;
            defs[i] = ProgramDef{names[i], ProgramClass::Plain, b[i].vertex.c_str(), b[i].fragment.c_str(),
                                 {"aPosition", "aNormal", "aTexCoord"}, level};
        }
        done = true;
    }
    return defs[static_cast<int>(kind)];
}

const ProgramDef& blitProgram() {
    static const ProgramDef def{"scene blit", ProgramClass::Plain, kBlitVertex, kBlitFragment, {"aPosition", "aTexCoord"}};
    return def;
}

const ProgramDef& wmoProgram(M2Kind kind) {
    static const char* const names[3] = {"WMO (opaque)", "WMO (alpha test)", "WMO (blend)"};
    static ProgramDef defs[3];
    static bool done = false;
    if (!done) {
        Built* b = wmoSources();
        for (int i = 0; i < 3; ++i) {
            defs[i] = ProgramDef{names[i], ProgramClass::Plain, b[i].vertex.c_str(), b[i].fragment.c_str(),
                                 {"aPosition", "aNormal", "aTexCoord", "aColor"}, i == static_cast<int>(M2Kind::AlphaTest) ? 3 : -1};
        }
        done = true;
    }
    return defs[static_cast<int>(kind)];
}

}  // namespace wowee::rendering::gl
