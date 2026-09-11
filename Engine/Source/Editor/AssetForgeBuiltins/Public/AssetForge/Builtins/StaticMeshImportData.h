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

		ASSETFORGEBUILTINS_API auto Validate(std::string& OutError) const -> bool;
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
		// Requires normalized source data and a state that passed its derived Validate.
		ASSETFORGEBUILTINS_API auto SetState(FStaticMeshImportDataState State) -> void;
		ASSETFORGEBUILTINS_API auto GetStaticMeshState() const
			-> FStaticMeshImportDataState;
		ASSETFORGEBUILTINS_API auto GetCompilationIdentity() const -> FXxHash128 override;
		ASSETFORGEBUILTINS_API auto Validate(std::string& OutError) const
			-> bool override;

	private:
		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;
	};
}
