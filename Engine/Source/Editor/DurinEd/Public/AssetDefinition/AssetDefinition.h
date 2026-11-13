#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/Object.h"
#include "AssetDefinition/AssetOpenTypes.h"
#include "AssetDefinition.gen.h"

namespace Durin
{
	class FAssetEditorToolkit;
	class FAssetEditorToolkitId;
	class FEditorDocumentManager;
	// Immutable type behavior; session, document and preview state belong to editors.
	DCLASS(Abstract)
	class DAssetDefinition : public DObject
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DAssetDefinition(const FObjectInitializer& Initializer);
		DURINED_API auto PostInitProperties() -> void override;
		DURINED_API auto BeginDestroy() -> void override;
		virtual auto GetAssetClassPath() const -> std::string { return {}; }
		virtual auto GetAssetDisplayName() const -> std::string { return {}; }
		virtual auto GetAssetColor() const -> std::array<float, 4> { return {0.5f, 0.5f, 0.5f, 1.0f}; }
		virtual auto GetAssetIcon() const -> std::string { return {}; }
		virtual auto GetAssetCategories() const -> std::vector<std::vector<std::string>> { return {}; }
		virtual auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport
		{ return {Args.OpenMethod, Args.OpenMethod == EAssetOpenMethod::Edit}; }
		// Dynamic-only definitions opt out; registration never creates a toolkit.
		virtual auto CanRegisterStatically() const -> bool { return true; }
		DURINED_API virtual auto OpenAssets(const FAssetOpenArgs& Args) const -> EAssetCommandResult;
	};

	// Supplies shared type metadata for definitions that use standard property editing.
	DCLASS(Abstract)
	class DAssetDefinitionDefault : public DAssetDefinition
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DAssetDefinitionDefault(const FObjectInitializer& Initializer);
		DURINED_API auto GetAssetDisplayName() const -> std::string override;
		DURINED_API auto GetAssetIcon() const -> std::string override;
		DURINED_API auto OpenAssets(const FAssetOpenArgs& Args) const -> EAssetCommandResult override;
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override
		{ return GetDefaultAssetOpenSupport(Args); }
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Other"}}; }
	protected:
		// Reflected editor identity; direct definitions return nullptr so mixed groups
		// cannot silently fall back to the generic reflected editor.
		DURINED_API virtual auto GetAssetEditorClass() const -> DClass*;
		// Implementation reuse for direct inspectors. Public opening remains OpenAssets;
		// the definition supplies its Toolkit identity and construction only.
		DURINED_API auto OpenAssetsWithToolkit(const FAssetOpenArgs& Args, const FAssetEditorToolkitId& ToolkitId,
			const std::function<std::shared_ptr<FAssetEditorToolkit>(FEditorDocumentManager&)>& CreateToolkit) const -> EAssetCommandResult;
	private:
		auto ExecuteOpenAssets(const FAssetOpenArgs& Args,
			std::variant<DClass*, const FAssetEditorToolkitId*> EditorType,
			const std::function<std::shared_ptr<FAssetEditorToolkit>(std::span<DObject* const>)>& CreateEditor) const -> EAssetCommandResult;
	};
}
