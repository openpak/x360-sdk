#version 450
// The pixel's place across a 1280x720 target as its colour, gl_FragCoord.x in red and .y in green: in a tiled pass the
// pixel parameters give the row in the band, and xsdk's constant adds the band's top row (docs/xsc.md).
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = vec4(gl_FragCoord.xy * vec2(1.0 / 1280.0, 1.0 / 720.0), v_color.b, 1.0);
}
