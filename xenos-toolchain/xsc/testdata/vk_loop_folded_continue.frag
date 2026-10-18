#version 450
// A continue on a condition the unroller folds: the rest of that pass doesn't run, the passes after it do
// (tests/test_loop_branches.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; } pc;

void main()
{
	vec4 sum = vec4(v_uv, 0.0, 0.0);
	for (int i = 0; i < 4; i++) {
		if (i == 1)
			continue;
		sum += pc.step * float(i + 1);
	}
	o_color = sum;
}
