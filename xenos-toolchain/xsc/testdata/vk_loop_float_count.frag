#version 450
// A loop whose count is a float a push constant holds, as int() takes it, continued past on data, its counter read
// after it: a hardware loop of 255 passes left on its condition, so a count past 255 stops at 255
// (tests/test_data_loops.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; float count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	int i = 0;
	for (; i < int(pc.count); i++) {
		if (float(i) == v_uv.x)
			continue;
		sum += pc.step;
	}
	o_color = vec4(sum.xyz, sum.w + float(i) * 0.03125);
}
