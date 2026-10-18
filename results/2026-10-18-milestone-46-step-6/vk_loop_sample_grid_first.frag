#version 450
// Samples in hardware loops' passes, for xsdk_test's raster grid and tests/test_loop_samples.py, each pixel's column
// and row in a 10-pixel cell picking its passes. The first loop marches a ray along a row of a height map, its step
// count from the pixel's place, and leaves once the ray passes the height it samples there, so the pixels of a group
// leave at different passes; each pass samples a mip chain whose levels are flat at a LOD the pass and the pixel give
// and at gradients the pass gives, and reads a tint at the loop's counter, through a0. The second, its count the same
// across each 2x2 quad and from no sample, as an implicit LOD and a derivative want, samples the mip chain at the LOD
// the GPU works out from coordinates the pass scales, and takes their derivative.
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_heights;
layout(set = 0, binding = 1) uniform sampler2D u_levels;
layout(push_constant) uniform Push { vec4 tints[8]; } pc;

void main()
{
	// gl_FragCoord sits on a pixel's centre, so a tenth of it is never near a whole number as rcp rounds it.
	vec2 cell = floor(gl_FragCoord.xy - 10.0 * floor(gl_FragCoord.xy * 0.1));
	vec2 quad = floor(cell * 0.5);
	float start = cell.x + 2.0 * cell.y;
	vec3 sum = vec3(0.0);
	int i = 0;
	for (; i < int(4.0 + mod(cell.x + cell.y, 5.0)); i++) {
		if (textureLod(u_heights, vec2((start + float(i) + 0.5) / 64.0, 0.5), 0.0).r >= 1.0 - float(i + 1) * 0.125)
			break;
		float graded = exp2(mod(float(i) + cell.x, 4.0) + 0.25) / 128.0;
		sum.r += textureLod(u_levels, vec2(0.5), mod(float(i) + cell.x + cell.y, 4.0) + 0.25).r * 0.0625;
		sum.g += textureGrad(u_levels, vec2(0.5), vec2(graded, 0.0), vec2(0.0, graded)).g * 0.0625 + pc.tints[i].x;
	}
	for (int k = 0; k < int(mod(quad.y, 3.0) + 2.0); k++) {
		vec2 coord = gl_FragCoord.xy * (exp2(mod(float(k) + quad.x, 4.0)) / 128.0);
		sum.b += texture(u_levels, coord, 0.25).b * 0.125 + dFdx(coord).x * 2.0;
	}
	o_color = vec4(sum.r + float(i) * 0.03125, sum.gb, 1.0);
}
