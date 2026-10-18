#version 450
// Loops whose count is data, for xsdk_test's raster grid and tests/test_data_loops.py: each pixel's column and row in a
// 10-pixel cell, scaled by the push block, give it its own counts, 0 to 321, past 255 in a fifth of the cell: a for
// loop's from a float, a while's, left on data too, and a do/while's, continued past on data, each a hardware loop of
// 255 passes left on its condition. The counters after them are the colour.
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 place; } pc;

void main()
{
	// gl_FragCoord sits on a pixel's centre, so a tenth of it is never near a whole number as rcp rounds it.
	vec2 cell = floor(gl_FragCoord.xy - 10.0 * floor(gl_FragCoord.xy * 0.1));
	float n = (cell.x + 10.0 * cell.y) * pc.place.x;
	int i = 0;
	for (; i < int(n); i++) {
	}
	float t = 0.0;
	while (t < pc.place.y - n) {
		t += 1.0;
		if (t == cell.x * 25.0 + 40.0)
			break;
	}
	float k = 0.0;
	float s = 0.0;
	do {
		k += 1.0;
		if (k == cell.y + 2.0)
			continue;
		s += 1.0;
	} while (k < n * pc.place.z);
	o_color = vec4(float(i), t, s, 255.0) * (1.0 / 255.0);
}
