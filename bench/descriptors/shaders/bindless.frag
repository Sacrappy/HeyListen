#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(push_constant) uniform PushConstants {
    uint index[30];
} pc;

void main() {
    vec4 c = vec4(0.0);
    for (int i = 0; i < 30; ++i) {
        c += texture(textures[pc.index[i]], vUV);
    }
    oColor = c / 30.0;
}
