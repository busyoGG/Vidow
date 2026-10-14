/* round — 圆角 + 1px 描边，适合做常驻（shader_idle）效果。
 *
 * 角是**超椭圆**（squircle，n = 4）不是圆弧：和窗口装饰（src/fx.c 的
 * sd_round_box，配置键 corner_n 默认 4）、Alt+Tab 的 MRU 卡片（src/mru.c 的
 * MRU_SQUIRCLE_N）、portal 选源对话框（tools/vidowportal.c 的 SQUIRCLE_N）
 * 同一种形状 —— 这几处都是 4.0，改要一起改。自定义 shader 这一路没有
 * uniform，指数就写死在这儿（和默认值一致）。 */
void main() {
	vec2 p = (v_uv - 0.5) * u_size;
	vec2 half_size = u_size * 0.5 - vec2(1.0);
	float r = min(14.0, min(half_size.x, half_size.y));
	const float n = 4.0;
	vec2 q = abs(p) - half_size + r;
	vec2 c = max(q, 0.0);
	float s = pow(c.x, n) + pow(c.y, n);
	/* 1 / |∇d|：超椭圆不是严格的 SDF（梯度模沿直边是 1、沿对角线是 2^(1/n)），
	 * 除掉之后 smoothstep 的斜坡宽度和方向无关。c 有个分量是 0 时它恒等于 1,
	 * 直边和圆弧完全一样。 */
	float den = length(vec2(pow(c.x, n - 1.0), pow(c.y, n - 1.0)));
	float k = den > 1e-4 ? pow(s, (n - 1.0) / n) / den : 1.0;
	float d = (pow(s, 1.0 / n) - r) * k + min(max(q.x, q.y), 0.0);
	float mask = 1.0 - smoothstep(-1.5, 1.5, d);
	vec4 px = texture2D(u_texture, v_uv) * mask;
	gl_FragColor = px * u_alpha;
}
