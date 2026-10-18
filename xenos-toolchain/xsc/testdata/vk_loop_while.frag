#version 450
// Loops whose count is data: a while on an interpolant, left on data too, then a do/while continued past on data, each
// a hardware loop of 255 passes left on its condition, the two sharing one loop constant (tests/test_data_loops.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	float t = 0.0;
	while (t < v_uv.x) {
		t += 1.0;
		if (t == v_uv.y)
			break;
		sum.xy += pc.step.xy;
	}
	float k = 0.0;
	do {
		k += 1.0;
		if (k == v_uv.y)
			continue;
		sum.zw += pc.step.zw;
	} while (k < v_uv.x * 0.5);
	o_color = sum + vec4(t, k, 0.0, 0.0) * 0.0625;
}
