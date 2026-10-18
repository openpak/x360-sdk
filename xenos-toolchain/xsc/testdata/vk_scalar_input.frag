#version 450
// A scalar interpolant read as a value outside a hardware loop: scaled, chosen on as an int, and the int an unrolled
// loop breaks at. It holds its value in x alone, and each read its whole register (tests/test_scalar_inputs.py).
layout(location = 0) in float v_s;
layout(location = 0) out vec4 o_color;

void main()
{
	vec4 sum = vec4(v_s * 0.25);
	sum += int(v_s) > 1 ? vec4(0.5) : vec4(0.125);
	for (int i = 0; i < 4; i++) {
		if (i == int(v_s))
			break;
		sum += vec4(0.0625);
	}
	o_color = sum;
}
