#pragma once

#include "CoreMinimal.h"
#include "StaticMeshEditorAPI.h"
#include "AssetDefinition/AssetDefinition.h"
#include "StaticMeshAssetDefinitions.gen.h"

namespace Durin::StaticMeshEditor
{
	DCLASS()
	class DStaticMeshAssetDefinition : public DAssetDefinitionDefault
	{
		GENERATED_BODY()
	public:
		explicit DStaticMeshAssetDefinition(const FObjectInitializer& Initializer);
		auto GetAssetClassPath() const -> std::string override;
		auto GetAssetDisplayName() const -> std::string override { return "StaticMesh"; }
		auto GetAssetIcon() const -> std::string override;
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Meshes"}}; }
		auto OpenAssets(const FAssetOpenArgs& Args) const -> EAssetCommandResult override;
	protected:
		auto GetAssetEditorClass() const -> DClass* override { return nullptr; }
	};
}
