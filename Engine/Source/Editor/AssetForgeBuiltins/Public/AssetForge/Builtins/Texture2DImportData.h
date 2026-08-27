#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"

#include "Texture2DImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	// Loads authored packages that predate common Texture2D import data.
	DCLASS()
	class DTexture2DImportData final : public DAssetImportData
	{
		GENERATED_BODY()

	public:
		explicit DTexture2DImportData(const FObjectInitializer& ObjectInitializer)
			: Super(ObjectInitializer) {}

	private:
		DPROPERTY()
		std::string DecoderId;

		DPROPERTY()
		uint32 DecoderVersion = 0;
	};
}
