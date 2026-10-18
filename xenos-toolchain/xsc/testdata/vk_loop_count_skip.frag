#version 450
// Loops a push constant's int counts, i from -3 and j by 2: hardware loops of their own passes, ceil((n - start) /
// step) to 255, which the runtime works out from the int, each its own loop constant; the second left on data too,
// and both counters read after (tests/test_data_loops.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; int count; } pc;

void main()
{
	vec2 sum = vec2(0.0);
	int i = -3;
	for (; i < pc.count; i++)
		sum.x += pc.step.x * float(i);
	int j = 0;
	for (; j < pc.count; j += 2) {
		sum.y += pc.step.y;
		if (float(j) >= v_uv.x)
			break;
	}
	o_color = vec4(sum, float(i) * 0.03125, float(j) * 0.03125);
}
