#pragma once

namespace zg::render {

// GLSL 330 vertex shader for raylib's DrawMeshInstanced. The `in mat4
// instanceTransform` attribute is wired up by raylib via
// SHADER_LOC_MATRIX_MODEL — caller must set that location after loading.
inline constexpr const char* kInstancingVS = R"GLSL(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in mat4 instanceTransform;

out vec2 fragTexCoord;
out vec4 fragColor;

uniform mat4 mvp;

void main() {
    fragTexCoord = vertexTexCoord;
    fragColor    = vertexColor;
    gl_Position  = mvp * instanceTransform * vec4(vertexPosition, 1.0);
}
)GLSL";

// Companion fragment shader for the instancing pass — just outputs the
// material's diffuse color for every fragment.
inline constexpr const char* kInstancingFS = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;

uniform vec4 colDiffuse;

out vec4 finalColor;

void main() {
    finalColor = colDiffuse;
}
)GLSL";

// CRT post-process fragment shader: chromatic aberration, scrolling
// scanlines, and vignette. Operates over fragTexCoord (0..1) when applied
// to a fullscreen draw of the scene RenderTexture. The `resolution` and
// `time` uniforms drive the per-frame look; caller is responsible for
// SetShaderValue-ing them each frame.
inline constexpr const char* kCrtFS = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 resolution;
uniform float time;

out vec4 finalColor;

void main() {
    vec2 uv = fragTexCoord;
    vec2 center = vec2(0.5, 0.5);

    // Chromatic aberration: separate R / B sampling offsets, growing with
    // distance from screen center.
    vec2 ab = (uv - center) * 0.0035;
    float r = texture(texture0, uv + ab).r;
    float g = texture(texture0, uv).g;
    float b = texture(texture0, uv - ab).b;
    vec3 col = vec3(r, g, b);

    // Scanlines: vertical sinusoid in pixel space, slowly scrolling.
    float scan = sin((uv.y * resolution.y + time * 18.0) * 1.4) * 0.5 + 0.5;
    col *= mix(0.78, 1.0, scan);

    // Vignette: darken everything outside the central 60%.
    float d = length(uv - center);
    float vignette = smoothstep(0.85, 0.35, d);
    col *= vignette;

    finalColor = vec4(col, 1.0) * colDiffuse;
}
)GLSL";

}  // namespace zg::render
