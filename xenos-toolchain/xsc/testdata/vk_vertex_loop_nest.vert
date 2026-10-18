#version 450
// A vertex shader's hardware loops nested 2 deep (tests/test_nested_loops.py): a while on a vertex input, left on data
// at its pass's end, around a loop a push constant counts, continued past and left on data; the while's counter read
// after.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_place;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 step; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	float t = 0.0;
	while (t < a_place.x) {
		t += 1.0;
		for (int i = 0; i < pc.count; i++) {
			if (float(i) == a_place.y)
				continue;
			sum += pc.step;
			if (float(i) + t >= a_place.z)
				break;
		}
		if (t == a_place.w)
			break;
	}
	v_color = vec4(sum.xyz, sum.w + t * 0.125);
	gl_Position = a_position;
}
