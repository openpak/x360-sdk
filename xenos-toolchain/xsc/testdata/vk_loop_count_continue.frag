#version 450
// A loop a uniform buffer counts, continued past on data at two places in its pass, and never left early
// (tests/test_hardware_loop_breaks.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Loop { vec4 step; int count; } u;

void main()
{
	vec4 sum = vec4(0.0);
	for (int i = 0; i < u.count; i++) {
		if (float(i) == v_uv.x)
			continue;
		sum += u.step;
		if (float(i) == v_uv.y)
			continue;
		sum.w += 0.0625;
	}
	o_color = sum;
}
