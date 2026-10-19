#version 450
// A corner at the vertex's rate and a place and a tint at the instance's, without gl_InstanceIndex: compiled instanced,
// the runtime points the place's and the tint's fetches at the instance (tests/test_instancing.py).
layout(location = 0) in vec4 a_corner;
layout(location = 2) in vec4 a_place;
layout(location = 3) in vec4 a_tint;
layout(location = 0) out vec4 v_color;

void main()
{
	gl_Position = a_corner + a_place;
	v_color = a_tint;
}
