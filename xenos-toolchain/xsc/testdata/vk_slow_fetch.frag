#version 450
// Two fetches far across a large texture, as GX's indirect texturing makes them, read in a later exec: the coordinates
// are worked out first, so the fetches share an exec and the work reading them comes in the next, where it picks the
// row a third fetch reads (xsdk:docs/HARDWARE.md, "Slow fetches across execs"). A texel's red is 16 times its row's
// class, its row mod 16, plus 8.
layout(location = 0) in vec4 v_scatter;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D s_rows;

void main()
{
    // x: the quad's class and a half; y: a column; z: 16 rows on for each whole.
    float row = v_scatter.x + 16.0 * floor(v_scatter.z);
    vec4 a = texture(s_rows, vec2(v_scatter.y, row) * (1.0 / 1024.0));
    vec4 b = texture(s_rows, vec2(v_scatter.y + 512.0, row) * (1.0 / 1024.0));
    o_color = texture(s_rows, vec2(v_scatter.y + 256.0, (a.r + b.r) * (255.0 / 32.0)) * (1.0 / 1024.0));
}
