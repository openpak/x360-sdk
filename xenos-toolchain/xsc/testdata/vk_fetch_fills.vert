#version 450
// Inputs narrower than the vectors built from them, the rest 0 or 1, which their fetches give with the optimiser on
// (tests/test_fetch_fills.py): in_pos's w and in_uv's z and w; in_nrm's w the first of its two constants; not in_color's
// w, a component it has.
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_nrm;
layout(location = 0) out vec4 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_light;
layout(push_constant) uniform Push { vec4 rows[4]; vec4 light; } pc;

void main()
{
	vec4 p = vec4(in_pos, 1.0);
	gl_Position = vec4(dot(pc.rows[0], p), dot(pc.rows[1], p), dot(pc.rows[2], p), dot(pc.rows[3], p));
	v_uv = vec4(in_uv, 0.0, 1.0);
	v_color = vec4(in_color.rgb, 1.0);
	v_light = vec2(dot(vec4(in_nrm, 0.0), pc.light), dot(vec4(in_nrm, 1.0), pc.light));
}
