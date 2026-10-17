#version 450
// A row read at index i, an if/else on a uniform whose first arm reads a row at index j, then rows at i again. A pixel
// shader's arms on a uniform go in execs on the predicate, so where the first arm ran a0 holds j after them, and the
// reads at i must load it again (xsdk:docs/HARDWARE.md, "a0 past if/else arms"). The loop has the unroller merge the
// if/else (xsdk:src/xsc_unroll.c, wanted).
layout(location = 0) in vec4 v_index;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Rows { vec4 colors[16]; vec4 tints[16]; vec4 mode; } u;

void main()
{
    int i = int(v_index.x);
    int j = int(v_index.y);
    vec4 base = u.colors[i];
    vec4 tint;
    if (u.mode.x > 0.5)
        tint = u.tints[j] * u.mode.y;
    else
        tint = u.tints[i] * u.mode.z;
    vec4 sum = base * tint;
    for (int k = 0; k < 2; k++)
        sum += u.colors[i];
    o_color = sum;
}
