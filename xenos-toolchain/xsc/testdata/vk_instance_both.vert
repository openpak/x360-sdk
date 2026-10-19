#version 450
// gl_InstanceIndex and a place at the instance's rate: the place moves the corner, the index is its depth and scales
// the colour, both at the vertex's rate (tests/test_instancing.py).
layout(location = 0) in vec4 a_corner;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec4 a_place;
layout(location = 0) out vec4 v_color;

void main()
{
	gl_Position = a_corner + a_place + vec4(0.0, 0.0, float(gl_InstanceIndex), 0.0);
	v_color = a_color * float(gl_InstanceIndex + 1);
}
