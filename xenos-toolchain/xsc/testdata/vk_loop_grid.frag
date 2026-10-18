#version 450
// Pixels leaving an unrolled loop at different passes, for xsdk_test's raster grid and tests/test_loop_branches.py:
// each pixel's column and row in a 10-pixel cell pick the pass it breaks out at, in arms of an if/else on data, and the
// pass it continues past; pass 6 breaks for every pixel still in the loop, a break the unroller folds.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
	vec2 place = mod(floor(gl_FragCoord.xy), 10.0);
	vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
	for (int i = 0; i < 8; i++) {
		if (i == 6)
			break;
		sum.r += 0.0625;
		if (float(i) == place.y)
			continue;
		if (place.x < 5.0) {
			sum.g += 0.0625;
			if (float(i) > place.x)
				break;
		} else if (float(i) + 3.0 > place.x) {
			break;
		}
		sum.b += float(i + 1) * 0.03125;
	}
	o_color = sum;
}
