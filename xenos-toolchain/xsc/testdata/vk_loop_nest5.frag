#version 450
// Hardware loops nested 5 deep, one past the 4 LOOP_START nests (xenia:src/xenia/gpu/ucode.h:131): refused
// (tests/test_nested_loops.py).
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	for (int a = 0; a < pc.count; a++) {
		for (int b = 0; b < pc.count; b++) {
			for (int c = 0; c < pc.count; c++) {
				for (int d = 0; d < pc.count; d++) {
					for (int e = 0; e < pc.count; e++)
						sum += pc.step;
				}
			}
		}
	}
	o_color = sum;
}
