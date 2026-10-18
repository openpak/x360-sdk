#version 450
// textureLod and textureGrad at coordinates worked out from another sample, so the work the fetch waits for comes
// after a fetch, while the LOD and gradients are ready sooner, one LOD with no work at all: the setters mustn't go out
// ahead of that work, nor anything wait between a setter and its fetch.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_offsets;
layout(set = 0, binding = 1) uniform sampler2D u_texture;

void main()
{
	const vec2 uv = texture(u_offsets, v_uv * 0.5).xy * 2.0;
	o_color = vec4(textureLod(u_texture, uv, v_uv.x * 8.0).x, textureLod(u_texture, uv, v_uv.y).y,
		textureGrad(u_texture, uv, v_uv * 0.5, v_uv.yx).zw);
}
