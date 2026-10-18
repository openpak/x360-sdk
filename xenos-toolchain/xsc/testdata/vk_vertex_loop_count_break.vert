#version 450
// A vertex shader's loop a push constant counts: continued past on data, left on data in an arm of an if/else on data,
// and its counter read after it (tests/test_hardware_loop_breaks.py).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_place;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 step; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	int i = 0;
	for (; i < pc.count; i++) {
		if (float(i) == a_place.y)
			continue;
		sum += pc.step;
		if (a_place.z > 0.5) {
			if (float(i) >= a_place.x)
				break;
		} else {
			sum.w += 0.0625;
		}
	}
	v_color = vec4(sum.xyz, sum.w + float(i) * 0.125);
	gl_Position = a_position;
}
