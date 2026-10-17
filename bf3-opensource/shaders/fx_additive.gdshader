// VFX2 additive particles (Emissive emitters: fire, flashes, sparks; unshaded; docs/design/m2a-fx2.md §4). See
// fx_common.gdshaderinc. No fog: fog would add the fog colour once per additive layer (a white wash on the oilfire).
shader_type spatial;
render_mode blend_add, depth_draw_never, cull_disabled, skip_vertex_transform, unshaded, fog_disabled;

#include "res://shaders/fx_common.gdshaderinc"
