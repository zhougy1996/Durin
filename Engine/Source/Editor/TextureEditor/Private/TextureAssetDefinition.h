#pragma once

#include "CoreMinimal.h"
#include "TextureEditorAPI.h"
#include "AssetDefinition/AssetDefinition.h"
#include "Toolkits/AssetEditor.h"
#include "TextureAssetDefinition.gen.h"

namespace Durin::TextureEditor
{
	// Texture sessions are constructed on demand rather than registered as shared workspaces.
	DCLASS(NoClassDefaultObject)
	class DTextureAssetEditor : public DAssetEditor
	{
		GENERATED_BODY()
	public:
		explicit DTextureAssetEditor(const FObjectInitializer& Initializer);
	protected:
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override;
	};

	// Shared type behavior; every opened texture owns its editing and preview state.
	DCLASS()
	class DTextureAssetDefinition : public DAssetDefinitionDefault
	{
		GENERATED_BODY()
	public:
		explicit DTextureAssetDefinition(const FObjectInitializer& Initializer);
		auto GetAssetClassPath() const -> std::string override;
		auto GetAssetDisplayName() const -> std::string override { return "Texture2D"; }
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Texture"}}; }
		protected:
		auto GetAssetEditorClass() const -> DClass* override;
	};
	DCLASS(NoClassDefaultObject)
	class DVolumeTextureAssetEditor : public DAssetEditor
	{
		GENERATED_BODY()
	public:
		explicit DVolumeTextureAssetEditor(const FObjectInitializer& Initializer);
	protected:
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override;
	};
	DCLASS()
	class DVolumeTextureAssetDefinition : public DAssetDefinitionDefault
	{
		GENERATED_BODY()
	public:
		explicit DVolumeTextureAssetDefinition(const FObjectInitializer& Initializer);
		auto GetAssetClassPath() const -> std::string override;
		auto GetAssetDisplayName() const -> std::string override { return "VolumeTexture"; }
		auto GetAssetIcon() const -> std::string override;
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Texture"}}; }
	protected:
		auto GetAssetEditorClass() const -> DClass* override;
	};
	DCLASS()
	class DTextureCubeAssetDefinition : public DAssetDefinitionDefault
	{
		GENERATED_BODY()
	public:
		explicit DTextureCubeAssetDefinition(const FObjectInitializer& Initializer);
		auto GetAssetClassPath() const -> std::string override;
		auto GetAssetDisplayName() const -> std::string override { return "Texture Cube"; }
		auto GetAssetIcon() const -> std::string override;
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Texture"}}; }
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override { return {Args.OpenMethod, Args.OpenMethod == EAssetOpenMethod::View}; }
	};

}
