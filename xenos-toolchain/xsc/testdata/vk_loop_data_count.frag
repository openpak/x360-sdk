#version 450
// A loop whose count is an interpolant, so the pixels of a group run different passes, left on data too: a hardware
// loop of 255 passes left on its condition (tests/test_data_loops.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	for (int i = 0; i < int(v_uv.x); i++) {
		sum += pc.step;
		if (sum.y > v_uv.y)
			break;
	}
	o_color = sum;
}
