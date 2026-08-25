#include "MaterialEditorModule.h"

#include "Editor/WorkspaceManager.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailService.h"
#include "Widgets/MMaterialEditor.h"

namespace Durin
{
	using namespace Editor::Material;

	IMPLEMENT_MODULE(FMaterialEditorModule, MaterialEditor)

	FMaterialEditorModule::~FMaterialEditorModule() = default;

	auto FMaterialEditorModule::StartupModule() -> void
	{
		EditorExtensionCallbacks =
			FModuleStartup::CreateOwnedCallbackRegistration("Editor.ExtensionRegistries");
		require(EditorExtensionCallbacks.IsValid());
	}

	auto FMaterialEditorModule::ShutdownModule() -> void
	{
		UnregisterMaterialEditor();
	}

	auto FMaterialEditorModule::RegisterMaterialEditor(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (MaterialThumbnailRegistration && MaterialThumbnailRegistration->IsValid())
			|| (MaterialInstanceThumbnailRegistration && MaterialInstanceThumbnailRegistration->IsValid()))
			return false;
		WorkspaceRegistration.reset();
		MaterialThumbnailRegistration.reset();
		MaterialInstanceThumbnailRegistration.reset();
		std::shared_ptr<MMaterialEditor> Workspace = std::make_shared<MMaterialEditor>(
			WorkspaceManager, EditorExtensionCallbacks.GetGate());
		::Durin::Editor::FWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {
				{
					.Descriptor = {
						.WorkspaceType = Workspace::Type,
						.DisplayName = "Material Editor",
						.RootKey = std::string(Workspace::RootKey),
						.bShowInWindowMenu = false,
						.bOpenByDefault = false,
						.DefaultHostDockPreference = ::Durin::Editor::EWorkspaceHostDockPreference::Center,
					},
					.Workspace = Workspace,
				},
			},
			.AssetEditors = {
				{
					.AssetClassName = DMaterial::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = Workspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::PerResource,
					.bClosable = true,
				},
				{
					.AssetClassName = DMaterialInstance::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = Workspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::PerResource,
					.bClosable = true,
				},
			},
		}, EditorExtensionCallbacks.GetGate());
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));
		std::string Error;
		auto MaterialHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FMaterialAssetThumbnailProvider>(
				DMaterial::StaticClass()->GetQualifiedName().ToString()),
			EditorExtensionCallbacks.GetGate(), Error);
		if (!MaterialHandle)
		{
			WorkspaceRegistration.reset();
			return false;
		}
		MaterialThumbnailRegistration =
			std::make_unique<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>(
				std::move(MaterialHandle));
		auto InstanceHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FMaterialAssetThumbnailProvider>(
				DMaterialInstance::StaticClass()->GetQualifiedName().ToString()),
			EditorExtensionCallbacks.GetGate(), Error);
		if (!InstanceHandle)
		{
			MaterialThumbnailRegistration.reset();
			WorkspaceRegistration.reset();
			return false;
		}
		MaterialInstanceThumbnailRegistration =
			std::make_unique<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>(
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
