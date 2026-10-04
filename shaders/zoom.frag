/* zoom — 从中心放大淡入；关闭时反向缩小。
 * 可用 uniform：u_texture u_progress u_phase u_size u_origin u_output_size
 *              u_alpha u_time u_flip_y  varying v_uv (0..1, 左上原点)
 * 输出必须是预乘 alpha。 */
void main() {
	float scale = mix(0.80, 1.0, u_progress);
	vec2 uv = (v_uv - 0.5) / scale + 0.5;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
		gl_FragColor = vec4(0.0);
		return;
	}
	gl_FragColor = texture2D(u_texture, uv) * u_alpha;
}
