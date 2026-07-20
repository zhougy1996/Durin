#include "MaterialEditorModule.h"

#include "Editor/EditorWorkspace.h"
#include "MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
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
		UnregisterMaterialEditorWorkspace();
	}

	auto FMaterialEditorModule::RegisterMaterialEditorWorkspace(FEditorWorkspaceManager& WorkspaceManager) -> bool
	{
		if (WorkspaceRegistration && WorkspaceRegistration->IsValid()) return false;
		WorkspaceRegistration.reset();
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
		return true;
	}

	auto FMaterialEditorModule::UnregisterMaterialEditorWorkspace() -> void
	{
		WorkspaceRegistration.reset();
	}
}
