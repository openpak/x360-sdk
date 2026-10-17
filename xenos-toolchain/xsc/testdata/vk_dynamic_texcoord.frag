#version 450
// A texture coordinate read at index i, scaled by 1 / w of a row read at index j first. The sample needs the read at i
// and not its a0 load, so fetch grouping must keep that read after the load, and past j's (xsdk:docs/HARDWARE.md, "a0
// through fetch grouping").
layout(location = 0) in vec4 v_index;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Rows { vec4 coords[16]; vec4 scales[16]; } u;
layout(set = 0, binding = 1) uniform sampler2D s_texels;

void main()
{
    int i = int(v_index.x);
    int j = int(v_index.y);
    float k = 1.0 / u.scales[j].w;
    o_color = texture(s_texels, vec2(k) * u.coords[i].xy);
}
