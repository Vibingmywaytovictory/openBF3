shader_type spatial;
render_mode unshaded, cull_back, shadows_disabled;

uniform sampler2D base_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D lit_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D noise_tex : filter_linear_mipmap, repeat_enable;
uniform sampler2D flow_tex : filter_linear_mipmap, repeat_disable;
uniform sampler2D mask_tex : filter_linear_mipmap, repeat_disable;
uniform float flow_speed = 0.02;
uniform float noise_scale = 1.5;
uniform float warp = 0.012;
uniform float glow_gain = 1.5;

void fragment() {
	vec2 flow = texture(flow_tex, UV).rg;
	float region = max(flow.r, flow.g);
	float t = TIME * flow_speed;
	vec2 drift_a = texture(noise_tex, UV * noise_scale + vec2(t, 0.5 * t)).rg - vec2(0.5);
	vec2 drift_b = texture(noise_tex, UV * noise_scale * 0.5 - vec2(0.7 * t, t)).rg - vec2(0.5);
	vec2 warped = UV + (drift_a + drift_b) * warp * region;
	float glow = clamp(texture(noise_tex, UV * noise_scale + vec2(0.0, -t)).r * glow_gain - 0.25, 0.0, 1.0);
	vec3 base = texture(base_tex, warped).rgb;
	vec3 lit = texture(lit_tex, warped).rgb;
	ALBEDO = mix(base, lit, glow * region) * texture(mask_tex, UV).r;
}
