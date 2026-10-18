#version 450
// Loops the push block's int n counts, i from 100 and j by 2, for xsdk_test's raster grid and tests/test_data_loops.py:
// hardware loops of their own passes, ceil((n - start) / step) to 255, which the runtime works out from the block. Each
// loop's passes and its counter after are the colour, scaled by the push block.
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 scale; int n; } pc;

void main()
{
	float a = 0.0;
	int i = 100;
	for (; i < pc.n; i++)
		a += 1.0;
	float b = 0.0;
	int j = 0;
	for (; j < pc.n; j += 2)
		b += 1.0;
	o_color = vec4(a, b, float(i + j) * 0.25, 255.0) * pc.scale.x;
}
