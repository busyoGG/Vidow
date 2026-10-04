/* round — 圆角 + 1px 描边，适合做常驻（shader_idle）效果。 */
void main() {
	vec2 p = (v_uv - 0.5) * u_size;
	vec2 half_size = u_size * 0.5 - vec2(1.0);
	float r = min(14.0, min(half_size.x, half_size.y));
	vec2 q = abs(p) - half_size + r;
	float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
	float mask = 1.0 - smoothstep(-1.5, 1.5, d);
	vec4 c = texture2D(u_texture, v_uv) * mask;
	gl_FragColor = c * u_alpha;
}
