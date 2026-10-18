#version 450
// A hardware loop in an arm of an if/else on a push constant, which becomes selects rather than execs on the predicate,
// as a loop isn't an exec (tests/test_nested_loops.py); the loop is left on data.
layout(location = 0) in vec4 v_place;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; vec4 mode; int count; } pc;

void main()
{
	vec4 sum = vec4(0.5);
	if (pc.mode.x > 0.5) {
		for (int i = 0; i < pc.count; i++) {
			if (float(i) >= v_place.x)
				break;
			sum += pc.step;
		}
	} else {
		sum = v_place * 0.25;
	}
	o_color = sum;
}
