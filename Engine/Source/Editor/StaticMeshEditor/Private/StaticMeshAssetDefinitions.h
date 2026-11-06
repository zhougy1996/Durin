#pragma once

#include "CoreMinimal.h"
#include "StaticMeshEditorAPI.h"
#include "AssetDefinition/AssetDefinition.h"
#include "Toolkits/AssetEditor.h"
#include "StaticMeshAssetDefinitions.gen.h"

namespace Durin::Editor::StaticMesh
{
	DCLASS(NoClassDefaultObject)
	class DStaticMeshAssetEditor : public DAssetEditor
	{
		GENERATED_BODY()
	public:
		explicit DStaticMeshAssetEditor(const FObjectInitializer& Initializer);
	protected:
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override;
	};
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
	protected:
		auto GetAssetEditorClass() const -> DClass* override;
	};
}
