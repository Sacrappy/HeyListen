#version 450
// A colour clear with a channel mask (the pipeline's write mask): float targets
layout(push_constant) uniform Push { vec4 color; } pc;
layout(location = 0) out vec4 o;
void main() { o = pc.color; }
