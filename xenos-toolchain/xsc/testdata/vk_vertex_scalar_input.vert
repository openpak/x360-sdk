#version 450
// A vertex shader's float attribute, which its fetch puts in x alone, scaled and as a loop's count carrying a vec4;
// read from its whole register, y, z and w took the fetch's other lanes (tests/test_scalar_inputs.py).
layout(location = 0) in vec4 a_position;
layout(location = 1) in float a_weight;
layout(location = 0) out vec4 v_color;

void main()
{
	vec4 sum = vec4(a_weight * 0.5);
	for (int i = 0; i < int(a_weight); i++)
		sum += vec4(0.125);
	v_color = sum;
	gl_Position = a_position;
}
