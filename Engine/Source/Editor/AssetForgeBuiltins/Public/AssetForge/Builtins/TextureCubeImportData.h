#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"
#include "Texture/TextureCube.h"

#include "TextureCubeImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	struct FTextureCubeImportDataState : AssetImport::FAssetImportDataState
	{
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		uint32 ProjectionVersion = 0;

		auto operator==(const FTextureCubeImportDataState&) const -> bool = default;
	};

	DCLASS()
	class DTextureCubeImportData final : public AssetImport::DAssetImportData
	{
		GENERATED_BODY()

	public:
		ASSETFORGEBUILTINS_API explicit DTextureCubeImportData(
			const FObjectInitializer& ObjectInitializer);
		auto GetSourceLayout() const -> ETextureCubeSourceLayout { return SourceLayout; }
		auto GetDecoderId() const -> std::string_view { return DecoderId; }
		auto GetDecoderVersion() const -> uint32 { return DecoderVersion; }
		auto GetProjectionVersion() const -> uint32 { return ProjectionVersion; }
		ASSETFORGEBUILTINS_API auto SetState(
			FTextureCubeImportDataState State, std::string& OutError) -> bool;
		ASSETFORGEBUILTINS_API auto GetTextureCubeState() const
			-> FTextureCubeImportDataState;
		auto GetState() const -> AssetImport::FAssetImportDataState override
		{
			return GetTextureCubeState();
		}
		ASSETFORGEBUILTINS_API auto Validate(std::string& OutError) const
			-> bool override;
		ASSETFORGEBUILTINS_API auto CloneToOwner(
			DObject* Owner, FName Name, std::string& OutError) const
			-> AssetImport::DAssetImportData* override;

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
