#version 450
// Hardware loops in both arms of an if/else on data, each run for every pixel and merged by selects
// (tests/test_nested_loops.py): a loop a push constant counts, left on data at its pass's end, and a while on data,
// continued past on data, its counter read after.
layout(location = 0) in vec4 v_place;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	if (v_place.x > 2.5) {
		for (int i = 0; i < pc.count; i++) {
			sum += pc.step;
			if (float(i) >= v_place.y)
				break;
		}
	} else {
		float t = 0.0;
		while (t < v_place.y) {
			t += 1.0;
			if (t == v_place.z)
				continue;
			sum += pc.step * 2.0;
		}
		sum.w += t * 0.0625;
	}
	o_color = sum;
}
