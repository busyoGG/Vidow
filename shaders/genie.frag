/* genie — 关闭时把窗口压成底边的一条窄带（u_phase == 2 时 u_progress 生效）。 */
void main() {
	float k = (u_phase == 2) ? u_progress : 0.0;
	float band = mix(1.0, 0.03, k);          /* 内容带的高度 */
	float vy = (v_uv.y - (1.0 - band)) / band;
	if (vy < 0.0 || vy > 1.0) {
		gl_FragColor = vec4(0.0);
		return;
	}
	float half_w = mix(0.5, 0.04, k * k);    /* 同时向中心收拢 */
	float vx = (v_uv.x - 0.5) * (0.5 / half_w) + 0.5;
	if (vx < 0.0 || vx > 1.0) {
		gl_FragColor = vec4(0.0);
		return;
	}
	gl_FragColor = texture2D(u_texture, vec2(vx, vy)) * u_alpha;
}
