#version 450
// A loop a push constant counts, left on data at the top of a pass, continued past on data, and left at a pass's end
// where the push block says, a break every pixel shares, gated as the pass's others are
// (tests/test_hardware_loop_breaks.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; float stop; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 0; i < pc.count; i++) {
		if (float(i) >= v_uv.x)
			break;
		sum += pc.step;
		if (float(i) == v_uv.y)
			continue;
		sum.w -= 0.0625;
		if (float(i) >= pc.stop)
			break;
	}
	o_color = sum;
}
