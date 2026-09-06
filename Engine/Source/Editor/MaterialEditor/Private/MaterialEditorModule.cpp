#include "MaterialEditorModule.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Icons/FontAwesomeIcons.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Asset.h"
#include "AssetTools/IAssetTools.h"
#include "Editor/WorkspaceManager.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transaction.h"
#include "MaterialAssetCreation.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Widgets/MMaterialEditor.h"

namespace Durin
{
	using namespace Editor::Material;
	namespace
	{
		template<typename TMaterial>
		auto CreateMaterialAsset(const FTopLevelAssetPath& AssetPath,
			std::string& OutError) -> bool
		{
			const FAssetToolsResult Created = IAssetTools::Get().CreateAsset(
				AssetPath, TMaterial::StaticClass());
			if (!Created)
			{
				OutError = Created.Message;
				return false;
			}
			const auto Discard = [&] {
				if (!IAssetTools::Get().DiscardPackage(Created.Package))
					OutError += " The unsaved material package could not be discarded.";
			};
			if constexpr (std::same_as<TMaterial, DMaterial>)
			{
				if (!PrepareNewMaterialForEditing(*Cast<DMaterial>(Created.Asset), OutError))
				{
					Discard();
					return false;
				}
			}
			const FAssetResult Result = SavePackage(Created.Package);
			if (!Result)
			{
				OutError = Result.Message;
				Discard();
				return false;
			}
			return true;
		}

	}

	IMPLEMENT_MODULE(FMaterialEditorModule, MaterialEditor)

	struct FMaterialEditorModule::FIntegrationState
	{
		std::vector<Editor::ContentBrowser::FScopedExtensionRegistration> TypePresentations;
		std::vector<Editor::ContentBrowser::FScopedExtensionRegistration>
			ContentBrowserExtensions;
	};

	FMaterialEditorModule::FMaterialEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}

	FMaterialEditorModule::~FMaterialEditorModule() = default;

	auto FMaterialEditorModule::ShutdownModule() -> void
	{
		UnregisterMaterialEditor();
		if (GEditor)
			checkf(GEditor->GetTransactor()
				->DiscardCustomChangesByModule("MaterialEditor"),
				"MaterialEditor cannot retire while one of its custom changes is active");
	}

	auto FMaterialEditorModule::RegisterMaterialEditor(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::DThumbnailManager& ThumbnailManager) -> bool
	{
		if ((WorkspaceRegistration && WorkspaceRegistration->IsValid())
			|| (MaterialThumbnailRegistration && MaterialThumbnailRegistration->IsValid())
			|| (MaterialInstanceThumbnailRegistration && MaterialInstanceThumbnailRegistration->IsValid()))
			return false;
		WorkspaceRegistration.reset();
		MaterialThumbnailRegistration.reset();
		MaterialInstanceThumbnailRegistration.reset();
		std::shared_ptr<MMaterialEditor> Workspace = std::make_shared<MMaterialEditor>(
			WorkspaceManager);
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
		});
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));
		std::string Error;
		auto MaterialHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DMaterialThumbnailRenderer>(
				DMaterial::StaticClass()->GetQualifiedName().ToString()),
			Error);
		if (!MaterialHandle)
		{
			WorkspaceRegistration.reset();
			return false;
		}
		MaterialThumbnailRegistration =
			std::make_unique<::Durin::Editor::FThumbnailRendererRegistrationHandle>(
				std::move(MaterialHandle));
		auto InstanceHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DMaterialThumbnailRenderer>(
				DMaterialInstance::StaticClass()->GetQualifiedName().ToString()),
			Error);
		if (!InstanceHandle)
		{
			MaterialThumbnailRegistration.reset();
			WorkspaceRegistration.reset();
			return false;
		}
		MaterialInstanceThumbnailRegistration =
			std::make_unique<::Durin::Editor::FThumbnailRendererRegistrationHandle>(
				std::move(InstanceHandle));
		const auto RegisterCreate = [this](std::string Id, std::string Label,
			std::string BaseName, bool bInstance) {
			std::string Error;
			auto Handle = ::Durin::Editor::ContentBrowser::RegisterAssetCreation({
				.Id = std::move(Id),
				.Label = std::move(Label),
				.DefaultName = std::move(BaseName),
				.Order = bInstance ? 210 : 200,
				.Create = bInstance ? CreateMaterialAsset<DMaterialInstance>
					: CreateMaterialAsset<DMaterial>,
				.AssetClassNameToOpen = (bInstance ? DMaterialInstance::StaticClass()
					: DMaterial::StaticClass())->GetQualifiedName().ToString(),
				}, Error);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register Content Browser material creation: {}", Error);
				return false;
			}
			Integration->ContentBrowserExtensions.push_back(std::move(Handle));
			return true;
		};
		if (!RegisterCreate("material.create-material", "Material", "NewMaterial", false)
			|| !RegisterCreate("material.create-instance", "Material Instance",
				"NewMaterialInstance", true))
		{
			Integration->ContentBrowserExtensions.clear();
			MaterialInstanceThumbnailRegistration.reset();
			MaterialThumbnailRegistration.reset();
			WorkspaceRegistration.reset();
			return false;
		}
		std::string PresentationError;
		{
			auto Handle = Editor::ContentBrowser::RegisterAssetTypePresentation({
				.AssetClassName = DMaterial::StaticClass()->GetQualifiedName().ToString(),
				.DisplayName = "Material",
				.Category = Editor::ContentBrowser::EAssetCategory::Material,
				.Icon = Icons::FileLines,
			}, PresentationError);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register browser type presentation: {}", PresentationError);
				UnregisterMaterialEditor();
				return false;
			}
			Integration->TypePresentations.push_back(std::move(Handle));
		}
		{
			auto Handle = Editor::ContentBrowser::RegisterAssetTypePresentation({
				.AssetClassName = DMaterialInstance::StaticClass()->GetQualifiedName().ToString(),
				.DisplayName = "Material Instance",
				.Category = Editor::ContentBrowser::EAssetCategory::Material,
				.Icon = Icons::FileLines,
			}, PresentationError);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register browser type presentation: {}", PresentationError);
				UnregisterMaterialEditor();
				return false;
			}
			Integration->TypePresentations.push_back(std::move(Handle));
		}
		return true;
	}

	auto FMaterialEditorModule::UnregisterMaterialEditor() -> void
	{
		Integration->TypePresentations.clear();
		Integration->ContentBrowserExtensions.clear();
		MaterialInstanceThumbnailRegistration.reset();
		MaterialThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}
}
