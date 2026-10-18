#version 450
// A hardware loop inside a loop of 3 passes the unroller unrolls, left on data, so the hardware loop is copied once a
// pass, the copies sharing the push constant's loop constant, each left on data too (tests/test_nested_loops.py).
layout(location = 0) in vec4 v_place;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	for (int k = 0; k < 3; k++) {
		if (v_place.x < float(k))
			break;
		for (int i = 0; i < pc.count; i++) {
			if (float(i) >= v_place.y + float(k))
				break;
			sum += pc.step * float(k + 1);
		}
	}
	o_color = sum;
}
