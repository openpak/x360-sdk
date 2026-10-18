#version 450
// Hardware loops nested 3 deep, for xsdk_test's raster grid and tests/test_nested_loops.py: each pixel's column and row
// in a 10-pixel cell, scaled and offset by the push block, give it its own passes at every level. A loop the push
// block's outer count counts, continued past on data, around a while on data, left on data, which leaves the while
// alone; in the while's pass an if/else on data, a checkerboard, holds a loop the inner count counts, left on data, in
// one arm and a do/while on data, sharing the while's loop constant, in the other. What each level ran is the colour.
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 place; int outer; int inner; } pc;

void main()
{
	// gl_FragCoord sits on a pixel's centre, so a tenth of it is never near a whole number as rcp rounds it.
	vec2 cell = floor(gl_FragCoord.xy - 10.0 * floor(gl_FragCoord.xy * 0.1));
	vec2 place = cell * pc.place.xy + pc.place.zw;
	float sum = place.x + place.y;
	float skip = sum - 4.0 * floor(sum * 0.25);
	float checker = sum - 2.0 * floor(sum * 0.5);
	float a = 0.0;
	float b = 0.0;
	float c = 0.0;
	for (int i = 0; i < pc.outer; i++) {
		if (float(i) == skip)
			continue;
		float t = 0.0;
		while (t < place.x * 0.5 + float(i)) {
			t += 1.0;
			if (t == place.y - float(i))
				break;
			if (checker > 0.5) {
				for (int j = 0; j < pc.inner; j++) {
					if (float(j) >= place.y - t)
						break;
					c += 1.0;
				}
			} else {
				float k = 0.0;
				do {
					k += 1.0;
					b += 1.0;
				} while (k < t * 0.25 + place.x * 0.25 - place.y * 0.5);
			}
		}
		a += t;
	}
	o_color = vec4(a * 0.03125, b * 0.015625, c * 0.015625, 1.0);
}
