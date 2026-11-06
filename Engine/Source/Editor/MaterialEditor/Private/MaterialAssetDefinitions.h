#pragma once

#include "CoreMinimal.h"
#include "MaterialEditorAPI.h"
#include "AssetDefinition/AssetDefinition.h"
#include "Toolkits/AssetEditor.h"
#include "MaterialAssetDefinitions.gen.h"

namespace Durin::Editor::Material
{
	inline auto GetMaterialGraphOpenSupport(const FAssetOpenSupportArgs& Args) -> FAssetOpenSupport
	{ return {Args.OpenMethod, Args.OpenMethod == EAssetOpenMethod::Edit}; }
	DCLASS(NoClassDefaultObject)
	class DMaterialAssetEditor : public DAssetEditor
	{
		GENERATED_BODY()
	public:
		explicit DMaterialAssetEditor(const FObjectInitializer& Initializer);
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override { return GetMaterialGraphOpenSupport(Args); }
	protected:
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override;
	};
	DCLASS(NoClassDefaultObject)
	class DMaterialFunctionAssetEditor : public DAssetEditor
	{
		GENERATED_BODY()
	public:
		explicit DMaterialFunctionAssetEditor(const FObjectInitializer& Initializer);
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override { return GetMaterialGraphOpenSupport(Args); }
	protected:
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override;
	};
	DCLASS(NoClassDefaultObject)
	class DMaterialParameterCollectionAssetEditor : public DAssetEditor
	{
		GENERATED_BODY()
	public:
		explicit DMaterialParameterCollectionAssetEditor(const FObjectInitializer& Initializer);
	protected:
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override;
	};
	DCLASS()
	class DMaterialAssetDefinition : public DAssetDefinitionDefault
	{
		GENERATED_BODY()
	public:
		explicit DMaterialAssetDefinition(const FObjectInitializer& Initializer);
		auto GetAssetClassPath() const -> std::string override;
		auto GetAssetDisplayName() const -> std::string override { return "Material"; }
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Material"}}; }
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override { return GetMaterialGraphOpenSupport(Args); }
	protected:
		auto GetAssetEditorClass() const -> DClass* override;
	};
	DCLASS()
	class DMaterialInstanceAssetDefinition : public DAssetDefinitionDefault
	{
		GENERATED_BODY()
	public:
		explicit DMaterialInstanceAssetDefinition(const FObjectInitializer& Initializer);
		auto GetAssetClassPath() const -> std::string override;
		auto GetAssetDisplayName() const -> std::string override { return "Material Instance"; }
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Material"}}; }
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override { return GetMaterialGraphOpenSupport(Args); }
	protected:
		auto GetAssetEditorClass() const -> DClass* override;
	};
	DCLASS()
	class DMaterialFunctionAssetDefinition : public DAssetDefinitionDefault
	{
		GENERATED_BODY()
	public:
		explicit DMaterialFunctionAssetDefinition(const FObjectInitializer& Initializer);
		auto GetAssetClassPath() const -> std::string override;
		auto GetAssetDisplayName() const -> std::string override { return "Material Function"; }
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Material"}}; }
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override { return GetMaterialGraphOpenSupport(Args); }
	protected:
		auto GetAssetEditorClass() const -> DClass* override;
	};
	DCLASS()
	class DMaterialParameterCollectionAssetDefinition : public DAssetDefinitionDefault
	{
		GENERATED_BODY()
	public:
		explicit DMaterialParameterCollectionAssetDefinition(const FObjectInitializer& Initializer);
		auto GetAssetClassPath() const -> std::string override;
		auto GetAssetDisplayName() const -> std::string override { return "Material Parameter Collection"; }
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return {{"Material"}}; }
	protected:
		auto GetAssetEditorClass() const -> DClass* override;
	};
}
