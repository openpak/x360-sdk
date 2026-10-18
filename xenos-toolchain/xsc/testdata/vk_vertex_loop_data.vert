#version 450
// A vertex shader's loop whose count is a vertex input, a while stepping its counter in its pass, continued past on
// data, and the counter after it: a hardware loop of 255 passes left on its condition (tests/test_data_loops.py).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_place;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 step; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	int i = 0;
	while (i < int(a_place.x)) {
		i++;
		if (float(i) == a_place.y)
			continue;
		sum += pc.step;
	}
	v_color = vec4(sum.xyz, sum.w + float(i) * 0.125);
	gl_Position = a_position;
}
