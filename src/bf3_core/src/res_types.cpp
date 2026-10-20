#include "bf3/core/res_types.h"

#include <cstdio>

namespace bf3::core {

std::string res_type_name(uint32_t p_type) {
	switch (p_type) {
		case 0xC6DBEE07u:
			return "AnimatedPointCloud";
		case 0xD070EED1u:
			return "AnimTrackData";
		case 0x51A3C853u:
			return "AssetBank";
		case 0xE565EB15u:
			return "DxShaderDatabase";
		case 0x10F0E5A1u:
			return "DxShaderProgramDatabase";
		case 0x5C4954A6u:
			return "DxTexture";
		case 0x70C5CB3Eu:
			return "EnlightenDatabase";
		case 0xE156AF73u:
			return "EnlightenProbeSet";
		case 0x59CEEB57u:
			return "EnlightenShaderDatabase";
		case 0x5BDFDEFEu:
			return "EnlightenSystem";
		case 0x4864737Bu:
			return "HavokDestructionPhysicsData";
		case 0x91043F65u:
			return "HavokPhysicsData";
		case 0xC78B9D9Du:
			return "ImpulseResponse";
		case 0x36F3F2C0u:
			return "IShaderDatabase";
		case 0xC417BBD3u:
			return "ITexture";
		case 0x49B156D4u:
			return "MeshSet";
		case 0x30B4A553u:
			return "OccluderMesh";
		case 0x319D8CD0u:
			return "RagdollResource";
		case 0x3568E2B7u:
			return "RawFileData";
		case 0x41D57E10u:
			return "RenderTexture";
		case 0x7AEFC446u:
			return "StaticEnlightenDatabase";
		case 0x2D47A5FFu:
			return "SwfMovie";
		case 0x6BB6D7D2u:
			return "Terrain";
		case 0x15E1F32Eu:
			return "TerrainDecals";
		case 0x22FE8AC8u:
			return "TerrainStreamingTree";
		case 0x1CA38E06u:
			return "VisualTerrain";
		default: {
			char text[16];
			std::snprintf(text, sizeof(text), "0x%08X", p_type);
			return text;
		}
	}
}

}
