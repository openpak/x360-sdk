#version 450
// textureGrad in a pass of a loop whose count is data, at gradients the pass works out, so the pixels of a group run
// different passes (tests/test_loop_samples.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main()
{
	vec4 sum = vec4(0.0);
	vec2 gradient = vec2(0.0625, 0.0);
	for (int i = 0; i < int(v_uv.x); i++) {
		sum += textureGrad(u_texture, v_uv, gradient, gradient.yx * v_uv.y);
		gradient *= 2.0;
	}
	o_color = sum;
}
