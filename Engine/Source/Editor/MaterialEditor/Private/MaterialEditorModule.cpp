#include "MaterialEditorModule.h"

#include "ContentBrowser/ContentBrowserContracts.h"

#include "Asset/AssetOperations.h"
#include "Asset.h"
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
		auto CreateMaterialAsset(std::string_view VirtualDirectory,
			std::string_view BaseName, std::string& OutPath,
			std::string& OutClassName, std::string& OutError) -> bool
		{
			std::string Directory(VirtualDirectory);
			if (!Directory.ends_with('/')) Directory += '/';
			FPackagePath Path;
			for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
			{
				const std::string Name = Suffix == 0
					? std::string(BaseName)
					: std::format("{}{}", BaseName, Suffix + 1);
				if (!FPackagePath::TryCreate(Directory + Name, Path)
					|| Asset::FindAssetExact(Path)
					|| Asset::FindResidentPackage(Path)) continue;
				TMaterial* Material = nullptr;
				Asset::FAssetResult Result = Asset::CreateAsset(Path, Material);
				if (!Result || !Material)
				{
					OutError = Result ? "Could not create the material asset." : Result.Message;
					return false;
				}
				if constexpr (std::same_as<TMaterial, DMaterial>)
				{
					if (!PrepareNewMaterialForEditing(*Material, OutError))
					{
						Asset::UnloadPackage(Path);
						return false;
					}
				}
				Result = Asset::SavePackage(Material->GetPackage());
				if (!Result)
				{
					Asset::UnloadPackage(Path);
					OutError = Result.Message;
					return false;
				}
				OutPath = Path.ToString();
				OutClassName = Material->GetClass()->GetQualifiedName().ToString();
				return true;
			}
			OutError = "Could not find a unique material asset name in this folder.";
			return false;
		}
	}

	IMPLEMENT_MODULE(FMaterialEditorModule, MaterialEditor)

	struct FMaterialEditorModule::FIntegrationState
	{
		std::vector<Editor::ContentBrowser::FScopedExtensionRegistration>
			ContentBrowserExtensions;
	};

	FMaterialEditorModule::FMaterialEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}

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
		auto MaterialHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DMaterialThumbnailRenderer>(
				DMaterial::StaticClass()->GetQualifiedName().ToString()),
			EditorExtensionCallbacks.GetGate(), Error);
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
			EditorExtensionCallbacks.GetGate(), Error);
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
			auto Handle = ::Durin::Editor::ContentBrowser::RegisterExtension({
				.Id = std::move(Id),
				.Label = std::move(Label),
				.Category = ::Durin::Editor::ContentBrowser::EExtensionCategory::Create,
				.Order = bInstance ? 210 : 200,
				.IsApplicable = [](const auto& Context) {
					return !Context.VirtualDirectory.empty();
				},
				.Invoke = [BaseName = std::move(BaseName), bInstance](const auto& Invocation) {
					std::string Path;
					std::string ClassName;
					std::string Error;
					const bool bCreated = bInstance
						? CreateMaterialAsset<DMaterialInstance>(
							Invocation.Context.VirtualDirectory, BaseName,
							Path, ClassName, Error)
						: CreateMaterialAsset<DMaterial>(
							Invocation.Context.VirtualDirectory, BaseName,
							Path, ClassName, Error);
					if (!bCreated)
					{
						if (Invocation.ReportError)
							Invocation.ReportError(std::move(Error));
						return;
					}
					if (Invocation.NotifyMountedContentChanged)
						Invocation.NotifyMountedContentChanged();
					if (Invocation.RevealAsset) Invocation.RevealAsset(Path);
					if (Invocation.OpenAsset && !Invocation.OpenAsset(Path, ClassName)
						&& Invocation.ReportError)
						Invocation.ReportError(
							"The material was created, but its editor could not be opened.");
				},
				.OwnerGate = EditorExtensionCallbacks.GetGate(),
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
		return true;
	}

	auto FMaterialEditorModule::UnregisterMaterialEditor() -> void
	{
		Integration->ContentBrowserExtensions.clear();
		MaterialInstanceThumbnailRegistration.reset();
		MaterialThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
	}
}
