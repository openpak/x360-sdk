#version 450
// Parallax occlusion mapping's march, as a Shader Model 3 port has it: a step count from the view angle, so a loop
// whose count is data, sampling a depth map at gradients worked out before the loop, left once the ray passes the
// depth (tests/test_loop_samples.py).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_view;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_depths;
// The fewest steps, the most and the depth's scale.
layout(push_constant) uniform Push { vec4 march; } pc;

void main()
{
	vec3 view = normalize(v_view);
	float steps = mix(pc.march.y, pc.march.x, abs(view.z));
	float layer = 1.0 / steps;
	vec2 delta = view.xy * pc.march.z / (view.z * steps);
	vec2 dx = dFdx(v_uv);
	vec2 dy = dFdy(v_uv);
	vec2 uv = v_uv;
	float depth = 0.0;
	for (int i = 0; i < int(steps); i++) {
		if (depth >= textureGrad(u_depths, uv, dx, dy).r)
			break;
		uv -= delta;
		depth += layer;
	}
	o_color = vec4(uv, depth, 1.0);
}
