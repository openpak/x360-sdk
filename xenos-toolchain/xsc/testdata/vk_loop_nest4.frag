#version 450
// Hardware loops nested 4 deep (tests/test_nested_loops.py): loops a push constant counts at the first and third levels,
// sharing its loop constant, and loops on data at the second and fourth, sharing the fixed one; a continue on data at
// the third, a break on data at the fourth, which leaves it alone, and one ending the second's pass. The fourth level
// reads the push block's rows at the third's counter and the third at the first's, so a0 is loaded at each.
layout(location = 0) in vec4 v_place;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 rows[4]; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	for (int i = 0; i < pc.count; i++) {
		float t = 0.0;
		while (t < v_place.x) {
			t += 1.0;
			for (int j = 0; j < pc.count; j++) {
				if (float(j) == v_place.y + float(i))
					continue;
				float k = 0.0;
				do {
					k += 1.0;
					if (k > v_place.z + t)
						break;
					sum += pc.rows[j];
				} while (k < float(j + 2));
				sum.w += pc.rows[i].w;
			}
			if (t >= v_place.w)
				break;
		}
		sum.x += t * 0.125;
	}
	o_color = sum;
}
