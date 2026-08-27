#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"

#include "Texture2DImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	struct FTexture2DImportDataState : FAssetImportDataState
	{
		std::string DecoderId;
		uint32 DecoderVersion = 0;

		auto operator==(const FTexture2DImportDataState&) const -> bool = default;
	};

	// Owns the editor-only source interpretation needed to reimport one Texture2D.
	DCLASS()
	class DTexture2DImportData final : public DAssetImportData
	{
		GENERATED_BODY()

	public:
		ASSETFORGEBUILTINS_API explicit DTexture2DImportData(
			const FObjectInitializer& ObjectInitializer);

		auto GetDecoderId() const -> std::string_view { return DecoderId; }
		auto GetDecoderVersion() const -> uint32 { return DecoderVersion; }
		ASSETFORGEBUILTINS_API auto SetState(
			FTexture2DImportDataState State, std::string& OutError) -> bool;
		ASSETFORGEBUILTINS_API auto GetTexture2DState() const
			-> FTexture2DImportDataState;
		auto GetState() const -> FAssetImportDataState override
		{
			return GetTexture2DState();
		}
		ASSETFORGEBUILTINS_API auto Validate(std::string& OutError) const
			-> bool override;
		ASSETFORGEBUILTINS_API auto CloneToOwner(
			DObject* Owner, FName Name, std::string& OutError) const
			-> DAssetImportData* override;

	private:
		DPROPERTY()
		std::string DecoderId;

		DPROPERTY()
		uint32 DecoderVersion = 0;
	};
}
