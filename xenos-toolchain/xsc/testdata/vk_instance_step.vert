#version 450
// xsdk milestone 47 step 3's strips and points (xsdk:tests/test_instancing.c): a corner and a colour at the vertex's
// rate alone, the corner moved by gl_InstanceIndex times the step its z and w hold, and blue gl_InstanceIndex mod 64
// times 4, a quarter into its step.
layout(location = 0) in vec4 a_corner;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;

void main()
{
	float instance = float(gl_InstanceIndex);
	gl_Position = vec4(a_corner.xy + instance * a_corner.zw, 0.0, 1.0);
	v_color = vec4(a_color.rg, (fract(instance * (1.0 / 64.0)) * 256.0 + 0.25) * (1.0 / 255.0), 1.0);
}
