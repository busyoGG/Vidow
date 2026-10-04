/* wave — 横向正弦抖动，随动画收敛。 */
void main() {
	float amp = 0.03 * (1.0 - u_progress);
	vec2 uv = v_uv;
	uv.x += sin(uv.y * 12.0 + u_time * 14.0) * amp;
	if (uv.x < 0.0 || uv.x > 1.0) {
		gl_FragColor = vec4(0.0);
		return;
	}
	gl_FragColor = texture2D(u_texture, uv) * u_alpha;
}
