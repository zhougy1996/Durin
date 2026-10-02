#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"

#include "SceneImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	ASSETFORGEBUILTINS_API auto IsOneTimeModelSource(std::string_view Path) -> bool;
	ASSETFORGEBUILTINS_API auto IsOneTimeModelImportedAsset(const DObject& Object) -> bool;
	inline constexpr std::string_view ModelOneTimeImportDiagnostic =
		"glTF / GLB / FBX assets support one-time import only. Import into a new destination instead of reimporting.";

	// Retains readable scene-output provenance; this receipt does not authorize
	// source reimport. Family source metadata remains in the common base receipt.
	DCLASS()
	class DSceneImportData final : public DAssetImportData
	{
		GENERATED_BODY()

	public:
		explicit DSceneImportData(const FObjectInitializer& ObjectInitializer)
			: Super(ObjectInitializer) {}

		DPROPERTY()
		std::string SourceIdentity;

		DPROPERTY()
		std::string OutputIdentity;
	};
}
