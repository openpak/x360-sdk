#version 450
// dFdx and dFdy in a pass of a loop whose count is data, of a value the pass works out, v_count.x the same across each
// 2x2 quad: GLSL leaves a derivative undefined where a quad's pixels take different passes
// (tests/test_loop_samples.py).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_count;
layout(location = 0) out vec4 o_color;

void main()
{
	vec4 sum = vec4(0.0);
	vec2 p = v_uv;
	for (int i = 0; i < int(v_count.x); i++) {
		p = p * 2.0 + v_uv;
		sum += vec4(dFdx(p), dFdy(p));
	}
	o_color = sum;
}
