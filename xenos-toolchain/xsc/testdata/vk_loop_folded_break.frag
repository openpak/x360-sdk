#version 450
// Breaks on conditions the unroller folds: the passes after each don't run, and an inner loop's break leaves only the
// inner loop (tests/test_loop_branches.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; } pc;

void main()
{
	vec4 sum = vec4(v_uv, 0.0, 0.0);
	for (int i = 0; i < 4; i++) {
		sum += pc.step;
		if (i == 2)
			break;
	}
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++) {
			if (j == i)
				break;
			sum.w += 0.0625;
		}
	}
	o_color = sum;
}
