#pragma once
#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "AssetForge/Builtins/AssetImport.h"
#include "AssetForgeBuiltinsAPI.h"
#include "SceneImportOptions.gen.h"
namespace Durin::AssetForge::Builtins
{
	// Typed scene inputs. Native preview state contains no borrowed UI callbacks.
	DCLASS()
	class DSceneImportOptions : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DSceneImportOptions(const FObjectInitializer& Initializer) : Super(Initializer) {}
		FStaticMeshImportSettings MeshSettings = FStaticMeshImportSettings::MakeYUpNegativeZForward();
		FAssetImportOptions Assets;
		FSceneMaterialImportOptions Materials;
	private:
		friend class FSceneImportPreview;
		friend class FSceneTaskOperation;
		std::shared_ptr<FSceneImportState> Prepared;
	};
}
