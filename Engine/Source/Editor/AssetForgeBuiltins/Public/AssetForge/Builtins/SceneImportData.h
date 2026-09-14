#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"

#include "SceneImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	// Identifies scene-owned mesh/texture outputs independently of filenames and
	// content hashes. Family source metadata remains in the common base receipt.
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
