shader_type spatial;
render_mode blend_premul_alpha, depth_draw_never, cull_disabled, skip_vertex_transform, diffuse_lambert_wrap, specular_disabled;

#define FX_LIT
#define FX_PREMUL
#include "res://shaders/fx_common.gdshaderinc"
