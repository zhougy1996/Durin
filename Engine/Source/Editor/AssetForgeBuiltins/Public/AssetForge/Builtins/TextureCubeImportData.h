#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"
#include "Texture/TextureCube.h"

#include "TextureCubeImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	// Loads authored packages that predate common TextureCube import data.
	DCLASS()
	class DTextureCubeImportData final : public DAssetImportData
	{
		GENERATED_BODY()

	public:
		explicit DTextureCubeImportData(const FObjectInitializer& ObjectInitializer)
			: Super(ObjectInitializer) {}

	private:
		DPROPERTY()
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;

		DPROPERTY()
		std::string DecoderId;

		DPROPERTY()
		uint32 DecoderVersion = 0;

		DPROPERTY()
		uint32 ProjectionVersion = 0;
	};
}
