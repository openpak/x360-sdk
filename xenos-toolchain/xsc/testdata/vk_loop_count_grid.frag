#version 450
// Pixels leaving a hardware loop a push constant counts at their own passes, for xsdk_test's raster grid and timed
// check and tests/test_hardware_loop_breaks.py: each pixel's column and row in a 10-pixel cell, scaled and offset by
// the push block, pick the pass it continues past, the pass it breaks at in an arm of an if/else on data, and the pass
// whose end it breaks at; the counter read after the loop adds to red.
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 place; int count; } pc;

void main()
{
	// gl_FragCoord sits on a pixel's centre, so a tenth of it is never near a whole number as rcp rounds it.
	vec2 cell = floor(gl_FragCoord.xy - 10.0 * floor(gl_FragCoord.xy * 0.1));
	vec2 place = cell * pc.place.xy + pc.place.zw;
	vec3 sum = vec3(0.0);
	int i = 0;
	for (; i < pc.count; i++) {
		sum.r += 0.0625;
		if (float(i) == place.x - 2.0)
			continue;
		if (place.x < 5.0) {
			sum.g += 0.0625;
			if (float(i) > place.x)
				break;
		}
		sum.b += float(i + 1) * 0.015625;
		if (float(i) >= place.y)
			break;
	}
	// The counter in red too, as xsdk_test checks red, green and blue.
	o_color = vec4(sum.r + float(i) * 0.03125, sum.gb, 1.0);
}
