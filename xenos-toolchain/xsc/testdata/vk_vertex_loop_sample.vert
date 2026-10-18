#version 450
// A vertex shader's textureLod in a pass of a loop whose count is a vertex input, at coordinates and a LOD the pass
// works out, left on what it samples (tests/test_loop_samples.py).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_place;
layout(location = 0) out vec4 v_color;
layout(set = 0, binding = 0) uniform sampler2D u_heights;

void main()
{
	vec4 sum = vec4(0.0);
	vec2 uv = a_place.zw;
	for (int i = 0; i < int(a_place.x); i++) {
		vec4 texel = textureLod(u_heights, uv, float(i));
		sum += texel;
		if (texel.x > a_place.y)
			break;
		uv.x += 0.125;
	}
	v_color = sum;
	gl_Position = a_position;
}
