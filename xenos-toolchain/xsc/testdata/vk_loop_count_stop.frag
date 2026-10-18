#version 450
// A loop a push constant counts, continued past on data, then left at a pass's end where the push block says: a break
// every pixel shares that a pixel which continued doesn't reach, so it's gated (tests/test_hardware_loop_breaks.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; float stop; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 0; i < pc.count; i++) {
		if (float(i) == v_uv.x)
			continue;
		sum += pc.step;
		sum.w -= 0.0625;
		if (float(i) >= pc.stop)
			break;
	}
	o_color = sum;
}
