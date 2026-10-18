#version 450
// Hardware loops nested 2 deep (tests/test_nested_loops.py): a loop a push constant counts, continued past on data,
// around a while on data left on data, which leaves the inner loop alone, then left on data at its pass's end, after
// the inner loop; both counters read after.
layout(location = 0) in vec4 v_place;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	int i = 0;
	for (; i < pc.count; i++) {
		if (float(i) == v_place.y)
			continue;
		float t = 0.0;
		while (t < v_place.x + float(i)) {
			t += 1.0;
			if (t == v_place.z)
				break;
			sum += pc.step;
		}
		sum.w += t * 0.0625;
		if (sum.x > v_place.w)
			break;
	}
	o_color = vec4(sum.xyz, sum.w + float(i) * 0.03125);
}
