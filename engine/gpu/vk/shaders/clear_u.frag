#version 450
layout(push_constant) uniform Push { uvec4 color; } pc;
layout(location = 0) out uvec4 o;
void main() { o = pc.color; }
