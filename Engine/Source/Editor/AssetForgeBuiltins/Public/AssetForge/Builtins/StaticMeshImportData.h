#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"
#include "StaticMesh/StaticMesh.h"

#include "StaticMeshImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	struct FStaticMeshImportDataState : FAssetImportDataState
	{
		FStaticMeshImportSettings ImportSettings;

		auto operator==(const FStaticMeshImportDataState&) const -> bool = default;
	};

	// Retains the axis interpretation required to rebuild a StaticMesh source.
	DCLASS()
	class DStaticMeshImportData final : public DAssetImportData
	{
		GENERATED_BODY()

	public:
		ASSETFORGEBUILTINS_API explicit DStaticMeshImportData(
			const FObjectInitializer& ObjectInitializer);
		auto GetImportSettings() const -> const FStaticMeshImportSettings&
		{
			return ImportSettings;
		}
		ASSETFORGEBUILTINS_API auto SetState(
			FStaticMeshImportDataState State, std::string& OutError) -> bool;
		ASSETFORGEBUILTINS_API auto GetStaticMeshState() const
			-> FStaticMeshImportDataState;
		ASSETFORGEBUILTINS_API auto Validate(std::string& OutError) const
			-> bool override;

	private:
		DPROPERTY()
		std::string ImporterId;

		DPROPERTY()
		uint32 ImporterVersion = 0;

		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;
	};
}
