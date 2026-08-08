#include "MaterialEditorModule.h"

#include "Editor/EditorWorkspace.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "Widgets/MMaterialEditor.h"

namespace Durin
{
	IMPLEMENT_MODULE(FMaterialEditorModule, MaterialEditor)

	FMaterialEditorModule::~FMaterialEditorModule() = default;

	auto FMaterialEditorModule::StartupModule() -> void
	{
	}

	auto FMaterialEditorModule::ShutdownModule() -> void
	{
		UnregisterMaterialEditor();
	}

	auto FMaterialEditorModule::RegisterMaterialEditor(
		FEditorWorkspaceManager& WorkspaceManager,
		FRenderedAssetThumbnailService& ThumbnailService) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (MaterialThumbnailRegistration && MaterialThumbnailRegistration->IsValid())
			|| (MaterialInstanceThumbnailRegistration && MaterialInstanceThumbnailRegistration->IsValid()))
			return false;
		WorkspaceRegistration.reset();
		MaterialThumbnailRegistration.reset();
		MaterialInstanceThumbnailRegistration.reset();
		std::shared_ptr<MMaterialEditor> Workspace = std::make_shared<MMaterialEditor>(WorkspaceManager);
		FEditorWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {
				{
					.Descriptor = {
						.WorkspaceType = MaterialEditorWorkspace::Type,
						.DisplayName = "Material Editor",
						.RootKey = std::string(MaterialEditorWorkspace::RootKey),
						.bShowInWindowMenu = false,
						.bOpenByDefault = false,
						.DefaultHostDockPreference = EEditorWorkspaceHostDockPreference::Center,
					},
					.Workspace = Workspace,
				},
			},
			.AssetEditors = {
				{
					.AssetClassName = DMaterial::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = MaterialEditorWorkspace::Type,
					.DocumentPolicy = EEditorDocumentPolicy::PerResource,
					.bClosable = true,
				},
				{
					.AssetClassName = DMaterialInstance::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = MaterialEditorWorkspace::Type,
					.DocumentPolicy = EEditorDocumentPolicy::PerResource,
					.bClosable = true,
				},
			},
		});
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<FEditorWorkspaceRegistrationHandle>(std::move(Registration));
		std::string Error;
		auto MaterialHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FMaterialAssetThumbnailProvider>(
				DMaterial::StaticClass()->GetQualifiedName().ToString()), Error);
		if (!MaterialHandle)
		{
			WorkspaceRegistration.reset();
			return false;
		}
		MaterialThumbnailRegistration =
			std::make_unique<FAssetThumbnailProviderRegistrationHandle>(
				std::move(MaterialHandle));
		auto InstanceHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FMaterialAssetThumbnailProvider>(
				DMaterialInstance::StaticClass()->GetQualifiedName().ToString()), Error);
		if (!InstanceHandle)
		{
			MaterialThumbnailRegistration.reset();
			WorkspaceRegistration.reset();
			return false;
		}
		MaterialInstanceThumbnailRegistration =
			std::make_unique<FAssetThumbnailProviderRegistrationHandle>(
				std::move(InstanceHandle));
		return true;
	}

	auto FMaterialEditorModule::UnregisterMaterialEditor() -> void
	{
		MaterialInstanceThumbnailRegistration.reset();
		MaterialThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}
}
