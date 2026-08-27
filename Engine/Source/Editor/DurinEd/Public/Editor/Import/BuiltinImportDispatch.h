#pragma once

namespace Durin::Editor
{
	enum class EBuiltinImportFamily : uint8
	{
		Texture,
		TerrainHeightmap,
		Scene,
		StaticMesh,
	};

	struct FBuiltinImportDescriptor
	{
		EBuiltinImportFamily Family;
		std::string_view Label;
		std::string_view Extensions;
	};

	inline constexpr std::array BuiltinImportDescriptors{
		FBuiltinImportDescriptor{EBuiltinImportFamily::Texture,
			"Texture...", ".png;.jpg;.jpeg;.bmp;.tga;.hdr"},
		FBuiltinImportDescriptor{EBuiltinImportFamily::TerrainHeightmap,
			"Terrain Heightmap...", ".png;.r16;.raw"},
		FBuiltinImportDescriptor{EBuiltinImportFamily::Scene,
			"Scene Source (FBX/glTF)...", ".fbx;.gltf;.glb"},
		FBuiltinImportDescriptor{EBuiltinImportFamily::StaticMesh,
			"Static Mesh (Geometry Only)...", ".fbx;.gltf;.glb;.obj"},
	};
}
