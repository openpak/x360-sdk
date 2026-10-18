#version 450
// A loop whose count is a scalar interpolant, carrying a vec4: the interpolant holds its value in x alone, and the
// count read from its whole register gated each component apart (tests/test_scalar_inputs.py).
layout(location = 0) in float v_count;
layout(location = 1) in vec4 v_step;
layout(location = 0) out vec4 o_color;

void main()
{
	vec4 sum = vec4(0.0);
	for (int i = 0; i < int(v_count); i++)
		sum += v_step;
	o_color = sum;
}
