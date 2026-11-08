#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2) uniform sampler2D tex2;
layout(set = 0, binding = 3) uniform sampler2D tex3;
layout(set = 0, binding = 4) uniform sampler2D tex4;
layout(set = 0, binding = 5) uniform sampler2D tex5;
layout(set = 0, binding = 6) uniform sampler2D tex6;
layout(set = 0, binding = 7) uniform sampler2D tex7;
layout(set = 0, binding = 8) uniform sampler2D tex8;
layout(set = 0, binding = 9) uniform sampler2D tex9;
layout(set = 0, binding = 10) uniform sampler2D tex10;
layout(set = 0, binding = 11) uniform sampler2D tex11;
layout(set = 0, binding = 12) uniform sampler2D tex12;
layout(set = 0, binding = 13) uniform sampler2D tex13;
layout(set = 0, binding = 14) uniform sampler2D tex14;
layout(set = 0, binding = 15) uniform sampler2D tex15;
layout(set = 0, binding = 16) uniform sampler2D tex16;
layout(set = 0, binding = 17) uniform sampler2D tex17;
layout(set = 0, binding = 18) uniform sampler2D tex18;
layout(set = 0, binding = 19) uniform sampler2D tex19;
layout(set = 0, binding = 20) uniform sampler2D tex20;
layout(set = 0, binding = 21) uniform sampler2D tex21;
layout(set = 0, binding = 22) uniform sampler2D tex22;
layout(set = 0, binding = 23) uniform sampler2D tex23;
layout(set = 0, binding = 24) uniform sampler2D tex24;
layout(set = 0, binding = 25) uniform sampler2D tex25;
layout(set = 0, binding = 26) uniform sampler2D tex26;
layout(set = 0, binding = 27) uniform sampler2D tex27;
layout(set = 0, binding = 28) uniform sampler2D tex28;
layout(set = 0, binding = 29) uniform sampler2D tex29;

void main() {
    vec4 c = texture(tex0,  vUV) + texture(tex1,  vUV) + texture(tex2,  vUV)
           + texture(tex3,  vUV) + texture(tex4,  vUV) + texture(tex5,  vUV)
           + texture(tex6,  vUV) + texture(tex7,  vUV) + texture(tex8,  vUV)
           + texture(tex9,  vUV) + texture(tex10, vUV) + texture(tex11, vUV)
           + texture(tex12, vUV) + texture(tex13, vUV) + texture(tex14, vUV)
           + texture(tex15, vUV) + texture(tex16, vUV) + texture(tex17, vUV)
           + texture(tex18, vUV) + texture(tex19, vUV) + texture(tex20, vUV)
           + texture(tex21, vUV) + texture(tex22, vUV) + texture(tex23, vUV)
           + texture(tex24, vUV) + texture(tex25, vUV) + texture(tex26, vUV)
           + texture(tex27, vUV) + texture(tex28, vUV) + texture(tex29, vUV);
    oColor = c / 30.0;
}
