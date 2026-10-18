#version 450
// texture() in a pass of a loop whose count is data, at the LOD the GPU works out from coordinates the pass scales,
// v_count.x the same across each 2x2 quad: GLSL leaves an implicit LOD undefined where a quad's pixels take different
// passes (tests/test_loop_samples.py).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_count;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main()
{
	vec4 sum = vec4(0.0);
	vec2 scale = vec2(1.0, 0.5);
	for (int i = 0; i < int(v_count.x); i++) {
		sum += texture(u_texture, v_uv * scale);
		scale *= 2.0;
	}
	o_color = sum;
}
