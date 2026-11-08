#version 450
layout(push_constant) uniform Push { ivec4 color; } pc;
layout(location = 0) out ivec4 o;
void main() { o = pc.color; }
