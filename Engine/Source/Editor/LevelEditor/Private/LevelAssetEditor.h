#pragma once
#include "CoreMinimal.h"
#include "LevelEditorAPI.h"
#include "AssetDefinition/AssetDefinition.h"
#include "Toolkits/AssetEditor.h"
#include "LevelEditorContentBrowserCallbacks.h"
#include "LevelAssetEditor.gen.h"

namespace Durin
{
	namespace LevelEditor { class FLevelEditorSessionSettings; }
	struct FLevelEditorHostContext
	{
		std::shared_ptr<LevelEditor::FLevelEditorSessionSettings> Settings;
		LevelEditor::FContentBrowserCallbacks Browser;
		std::function<void()> OpenProjectBrowser;
	};
	inline auto GetLevelAssetOpenSupport(const FAssetOpenSupportArgs& Args) -> FAssetOpenSupport
	{ return {Args.OpenMethod, Args.OpenMethod == EAssetOpenMethod::Edit, EToolkitMode::WorldCentric}; }
	// Specialized Level session delegates scene activation to the existing controller.
	DCLASS(NoClassDefaultObject)
	class DLevelAssetEditor : public DAssetEditor
	{
		GENERATED_BODY()
	public:
		explicit DLevelAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto ConfigureHost(std::shared_ptr<FLevelEditorHostContext> Context) -> void { require(!IsInitialized()); HostContext = std::move(Context); }
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override { return GetLevelAssetOpenSupport(Args); }
		auto SupportsRestore() const -> bool override { return false; }
	protected:
		auto AllowsEmptyAssets() const -> bool override { return true; }
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override;
	private:
		std::shared_ptr<FLevelEditorHostContext> HostContext;
	};
	DCLASS()
	class DLevelAssetDefinition : public DAssetDefinition
	{
		GENERATED_BODY()
	public:
		explicit DLevelAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override { return GetLevelAssetOpenSupport(Args); }
		auto GetAssetClassPath() const -> std::string override;
		auto GetAssetDisplayName() const -> std::string override { return "Level"; }
		auto GetAssetIcon() const -> std::string override;
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Level"}}; }
		auto OpenAssets(const FAssetOpenArgs& Args) const -> FAssetOpenResult override;
	};
}
