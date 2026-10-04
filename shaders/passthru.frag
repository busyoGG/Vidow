/* passthru — 恒等效果，只用来验证 shader 管线的方向/颜色是否正确。 */
void main() {
	gl_FragColor = texture2D(u_texture, v_uv) * u_alpha;
}
