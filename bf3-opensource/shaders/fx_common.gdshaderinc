// VFX2 particle shading shared by fx_alpha / fx_additive / fx_premultiplied (docs/design/m2a-fx2.md §4). One
// MultiMesh per batch (bf3/fx/render_prep.h): instance transform = the class's basis and the world position, COLOR =
// HDR colour and transparency, INSTANCE_CUSTOM = (params row, flipbook frame, packed alpha levels, alpha scale). The
// per-emitter constants come from fx_params (RGBA32F, 4 texels per row, kFxParamsRowsPerLine rows per line).
//
// The including shader defines FX_LIT (lit with Lambert wrap, ROUGHNESS = the emitter's wrap-around) or not
// (unshaded, additive), and FX_PREMUL for premultiplied output.

uniform int shader_class = 0; // 0 Saq, 1 Daq, 2 Waq, 3 Mesh (FxShaderClass)
uniform bool first_person = false; // the 1p pass: the viewmodel's FOV override and z-clip scale
uniform float fp_fov_override = 55.0;
uniform float fp_z_clip_scale = 0.25;
uniform bool soft_enabled = true;
uniform sampler2D base_atlas : source_color, filter_linear_mipmap, repeat_disable;
uniform sampler2D normal_atlas : hint_normal, filter_linear_mipmap, repeat_disable;
uniform sampler2D fx_params : filter_nearest, repeat_disable;
uniform sampler2D fx_depth : hint_depth_texture, filter_nearest, repeat_disable;

const int kFxParamsRowsPerLine = 64;

varying flat vec4 v_base; // base atlas uv min, size of the frame cell
varying flat vec4 v_normal; // normal atlas uv min, size of the frame cell (z == 0: no normal map)
varying flat vec4 v_alpha; // alpha min, alpha max, alpha scale, alpha exponent
varying flat vec4 v_light; // light multiplier, wrap-around, emissive, soft fade distance (m)

vec4 fx_row_texel(int p_row, int p_k) {
	return texelFetch(fx_params, ivec2((p_row % kFxParamsRowsPerLine) * 4 + p_k, p_row / kFxParamsRowsPerLine), 0);
}

void vertex() {
	int row = int(INSTANCE_CUSTOM.x + 0.5);
	vec4 base_rect = fx_row_texel(row, 0);
	vec4 flip = fx_row_texel(row, 1);
	vec4 normal_rect = fx_row_texel(row, 2);
	vec4 lighting = fx_row_texel(row, 3);

	// Flipbook cell (frames are not blended: the integer part of the frame), row-major in the entry.
	float count = max(flip.x, 1.0);
	float columns = clamp(flip.y, 1.0, count);
	float rows = ceil(count / columns);
	float f = mod(floor(max(INSTANCE_CUSTOM.y, 0.0)), count);
	vec2 cell = vec2(mod(f, columns), floor(f / columns));
	vec2 cell_size = vec2(1.0 / columns, 1.0 / rows);
	vec2 rect_min = base_rect.xy;
	vec2 rect_size = base_rect.zw;
	if (rect_size.x == 0.0 || rect_size.y == 0.0) {
		rect_min = vec2(0.0);
		rect_size = vec2(1.0); // nothing resolved, no default entry: the whole base texture
	}
	v_base = vec4(rect_min + cell * cell_size * rect_size, cell_size * rect_size);
	if (base_rect.z < 0.0) {
		v_base = vec4(0.0, 0.0, -1.0, -1.0); // kFxRectWhite
	}
	v_normal = normal_rect.z > 0.0 ? vec4(normal_rect.xy + cell * cell_size * normal_rect.zw, cell_size * normal_rect.zw) : vec4(0.0);

	float packed = INSTANCE_CUSTOM.z;
	float amin = floor(packed / 1024.0);
	v_alpha = vec4(amin / 1023.0, (packed - amin * 1024.0) / 1023.0, INSTANCE_CUSTOM.w, max(flip.z, 0.0));

	vec3 ax = MODEL_MATRIX[0].xyz;
	vec3 ay = MODEL_MATRIX[1].xyz;
	vec3 az = MODEL_MATRIX[2].xyz;
	vec3 origin = MODEL_MATRIX[3].xyz;
	float extent = 0.0;
	if (shader_class == 0) {
		// Screen-aligned: a 2-D basis in the camera plane.
		vec3 o = (VIEW_MATRIX * vec4(origin, 1.0)).xyz;
		VERTEX = o + vec3(ax.xy * VERTEX.x + ay.xy * VERTEX.y, 0.0);
		NORMAL = vec3(0.0, 0.0, 1.0);
		TANGENT = vec3(normalize(ax.xy + vec2(1e-9, 0.0)), 0.0);
		BINORMAL = vec3(normalize(ay.xy + vec2(0.0, 1e-9)), 0.0);
		extent = max(length(ax.xy), length(ay.xy));
	} else if (shader_class == 1) {
		// Direction-aligned: the quad's length along the velocity projected on the view plane, its width facing the camera.
		vec3 o = (VIEW_MATRIX * vec4(origin, 1.0)).xyz;
		vec3 dv = mat3(VIEW_MATRIX) * az;
		vec2 d = length(dv.xy) > 1e-5 ? normalize(dv.xy) : vec2(0.0, 1.0);
		vec2 n = vec2(d.y, -d.x);
		VERTEX = o + vec3(n * (VERTEX.x * ax.x) + d * (VERTEX.y * ay.y), 0.0);
		NORMAL = vec3(0.0, 0.0, 1.0);
		TANGENT = vec3(n, 0.0);
		BINORMAL = vec3(d, 0.0);
		extent = max(abs(ax.x), abs(ay.y));
	} else {
		// World-aligned quads and the mesh stand-in: the instance transform as given.
		VERTEX = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
		NORMAL = normalize((MODELVIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
		TANGENT = normalize((MODELVIEW_MATRIX * vec4(TANGENT, 0.0)).xyz);
		BINORMAL = normalize((MODELVIEW_MATRIX * vec4(BINORMAL, 0.0)).xyz);
		extent = max(length(ax), length(ay));
	}
	v_light = vec4(lighting.x, clamp(lighting.y, 0.0, 1.0), lighting.z, abs(flip.w) * extent);
	if (first_person) {
		if (!IN_SHADOW_PASS) {
			// The viewmodel's FOV (StandardMaterial3D's fov_override code).
			float flip_y = sign(PROJECTION_MATRIX[1][1]);
			float aspect = PROJECTION_MATRIX[1][1] / PROJECTION_MATRIX[0][0];
			float fov_f = flip_y / tan(fp_fov_override * PI / 360.0);
			PROJECTION_MATRIX[0][0] = fov_f / aspect;
			PROJECTION_MATRIX[1][1] = fov_f;
		}
		Z_CLIP_SCALE = fp_z_clip_scale;
		v_light.w = 0.0; // no soft fade against the world in the 1p pass
	}
}

void fragment() {
	vec4 tex = v_base.z < 0.0 ? vec4(1.0) : texture(base_atlas, v_base.xy + UV * v_base.zw);
	// Alpha levels [inferred]: remap by min / max, then erode by the scale with the exponent as the edge sharpness.
	float span = v_alpha.y - v_alpha.x;
	float a1 = abs(span) < 0.0005 ? step(v_alpha.x, tex.a) : clamp((tex.a - v_alpha.x) / span, 0.0, 1.0);
	float a = clamp((a1 - 1.0 + v_alpha.z) * v_alpha.w, 0.0, 1.0) * clamp(COLOR.a, 0.0, 1.0);
	if (soft_enabled && v_light.w > 0.0) {
		float depth = texture(fx_depth, SCREEN_UV).r;
		vec4 view = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
		float scene_z = view.z / view.w;
		a *= clamp((VERTEX.z - scene_z) / v_light.w, 0.0, 1.0);
	}
	if (a < 0.002) {
		discard;
	}
	vec3 rgb = tex.rgb * COLOR.rgb;
#ifdef FX_LIT
	if (v_normal.z > 0.0) {
		NORMAL_MAP = texture(normal_atlas, v_normal.xy + UV * v_normal.zw).rgb;
	}
	// Light multiplier [inferred]: the lit share of the colour; the rest is self-lit. Emissive rows are all self-lit.
	float lit = v_light.z > 0.5 ? 0.0 : clamp(v_light.x, 0.0, 1.0);
	ALBEDO = rgb * lit;
	EMISSION = rgb * (1.0 - lit);
	ROUGHNESS = max(v_light.y, 0.02);
#ifdef FX_PREMUL
	ALBEDO *= a;
	EMISSION *= a;
#endif
#else
	ALBEDO = rgb;
#endif
	ALPHA = a;
}
