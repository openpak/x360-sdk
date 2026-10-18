#version 450
// textureLod in a hardware loop's pass at a LOD the pass works out, left on what it samples, and in an arm of an
// if/else on a push constant, which stays selects as no fetch goes on the predicate (tests/test_loop_samples.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push { vec4 mode; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	float lod = v_uv.x;
	for (int i = 0; i < pc.count; i++) {
		vec4 texel = textureLod(u_texture, v_uv, lod);
		sum += texel;
		if (pc.mode.x > 0.5)
			sum.w += textureLod(u_texture, v_uv, lod + 0.5).x;
		if (texel.y > v_uv.y)
			break;
		lod += 1.0;
	}
	o_color = sum;
}
