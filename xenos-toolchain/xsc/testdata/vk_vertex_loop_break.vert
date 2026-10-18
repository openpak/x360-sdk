#version 450
// Breaks and continues in a vertex shader's unrolled loop: on data, at the pass a_place.x gives and past the pass
// a_place.y gives, and where the unroller folds them, pass 1 skipped and none past pass 4
// (tests/test_loop_branches.py).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_place;
layout(location = 0) out vec4 v_color;

void main()
{
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 0; i < 6; i++) {
		if (i == 1)
			continue;
		sum.x += 0.125;
		if (float(i) == a_place.y)
			continue;
		sum.y += float(i + 1) * 0.0625;
		if (float(i) == a_place.x)
			break;
		sum.z += 0.125;
		if (i == 4)
			break;
	}
	v_color = sum;
	gl_Position = a_position;
}
