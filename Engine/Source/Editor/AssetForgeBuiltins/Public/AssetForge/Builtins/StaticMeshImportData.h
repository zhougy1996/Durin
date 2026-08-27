#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"
#include "StaticMesh/StaticMesh.h"

#include "StaticMeshImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	struct FStaticMeshImportDataState : AssetImport::FAssetImportDataState
	{
		std::string ImporterId;
		uint32 ImporterVersion = 0;
		FStaticMeshImportSettings ImportSettings;

		auto operator==(const FStaticMeshImportDataState&) const -> bool = default;
	};

	DCLASS()
	class DStaticMeshImportData final : public AssetImport::DAssetImportData
	{
		GENERATED_BODY()

	public:
		ASSETFORGEBUILTINS_API explicit DStaticMeshImportData(
			const FObjectInitializer& ObjectInitializer);
		auto GetImporterId() const -> std::string_view { return ImporterId; }
		auto GetImporterVersion() const -> uint32 { return ImporterVersion; }
		auto GetImportSettings() const -> const FStaticMeshImportSettings&
		{
			return ImportSettings;
		}
		ASSETFORGEBUILTINS_API auto SetState(
			FStaticMeshImportDataState State, std::string& OutError) -> bool;
		ASSETFORGEBUILTINS_API auto GetStaticMeshState() const
			-> FStaticMeshImportDataState;
		auto GetState() const -> AssetImport::FAssetImportDataState override
		{
			return GetStaticMeshState();
		}
		ASSETFORGEBUILTINS_API auto Validate(std::string& OutError) const
			-> bool override;
		ASSETFORGEBUILTINS_API auto CloneToOwner(
			DObject* Owner, FName Name, std::string& OutError) const
			-> AssetImport::DAssetImportData* override;

	private:
		DPROPERTY()
		std::string ImporterId;

		DPROPERTY()
		uint32 ImporterVersion = 0;

		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;
	};
}
