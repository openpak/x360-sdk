#version 450
// A sample whose result reaches no output, as Dusklight's 6a05b1f88a016fb2.frag takes one: the optimiser drops its
// fetch and keeps its sampler.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 sampled = texture(source, in_uv);
    vec4 opaque = vec4(sampled.rgb, 1.0);
    o_color = vec4(in_uv, 0.5, opaque.a);
}
