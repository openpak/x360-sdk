#version 450
// xsdk milestone 47 step 3's grids of cells (xsdk:tests/test_instancing.c): a corner at the vertex's rate, a place and
// a colour at the instance's, and blue gl_InstanceIndex mod 64 times 4, a quarter into its step.
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec2 a_place;
layout(location = 2) in vec4 a_color;
layout(location = 0) out vec4 v_color;

void main()
{
	gl_Position = vec4(a_corner + a_place, 0.0, 1.0);
	v_color = vec4(a_color.rg, (fract(float(gl_InstanceIndex) * (1.0 / 64.0)) * 256.0 + 0.25) * (1.0 / 255.0), 1.0);
}
