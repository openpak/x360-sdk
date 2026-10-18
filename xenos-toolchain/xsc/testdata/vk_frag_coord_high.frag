#version 450
// gl_FragCoord beside a block reaching c252, the group of 4 xsdk keeps for gl_FragCoord's constant: refused.
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Rows { vec4 rows[253]; } u;

void main()
{
	o_color = u.rows[252] * gl_FragCoord.x;
}
