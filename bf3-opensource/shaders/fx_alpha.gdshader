// VFX2 alpha-blended particles (lit, Lambert wrap; docs/design/m2a-fx2.md §4). See fx_common.gdshaderinc.
shader_type spatial;
render_mode blend_mix, depth_draw_never, cull_disabled, skip_vertex_transform, diffuse_lambert_wrap, specular_disabled;

#define FX_LIT
#include "res://shaders/fx_common.gdshaderinc"
