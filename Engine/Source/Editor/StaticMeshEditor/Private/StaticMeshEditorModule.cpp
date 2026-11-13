#include "StaticMeshEditorModule.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Icons/FontAwesomeIcons.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"
#include "Import/StaticMeshImportDialog.h"

namespace Durin
{
	using namespace StaticMeshEditor;
	IMPLEMENT_MODULE(FStaticMeshEditorModule, StaticMeshEditor)

	struct FStaticMeshEditorModule::FIntegrationState
	{
		ContentBrowser::FScopedExtensionRegistration ImportExtension;
		std::unique_ptr<StaticMeshEditor::FStaticMeshImportDialog> ImportDialog;
		ContentBrowser::FScopedHostPresentation Presentation;
	};

	FStaticMeshEditorModule::FStaticMeshEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}
	FStaticMeshEditorModule::~FStaticMeshEditorModule() = default;

	auto FStaticMeshEditorModule::StartupModule() -> void
	{
		require(RegisterContributions(GetDefaultThumbnailManager()));
	}

	auto FStaticMeshEditorModule::ShutdownModule() -> void
	{
		UnregisterContributions();
	}

	auto FStaticMeshEditorModule::RegisterContributions(DThumbnailManager& ThumbnailManager) -> bool
	{
		check(!ThumbnailRegistration);
		auto ThumbnailHandle = ThumbnailManager.RegisterScoped(
				std::make_unique<DStaticMeshThumbnailRenderer>());
		if (!ThumbnailHandle)
		{
			UnregisterContributions();
			return false;
		}
		ThumbnailRegistration =
			std::make_unique<FThumbnailRendererRegistrationHandle>(
				std::move(*ThumbnailHandle));
		std::string Error;
		auto ImportExtension = ContentBrowser::RegisterExtension({
			.Id = "static-mesh.import-static-mesh",
			.Label = "Static Mesh (Geometry Only)...",
			.Category = ContentBrowser::EExtensionCategory::Import,
			.Order = 400,
			.Mutation = ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [](const auto& Context) {
				return !Context.VirtualDirectory.empty();
			},
			.Invoke = [this](const auto& Invocation) {
				Integration->Presentation.Bind(Invocation.Host);
				Integration->ImportDialog = std::make_unique<FStaticMeshImportDialog>(
					ContentBrowser::MakeImportDialogCallbacks(Invocation, {}));
				if (Integration->ImportDialog)
					Integration->ImportDialog->Open(
						Invocation.Context.VirtualDirectory);
			},
			.DrawHostPresentation = [this](const ContentBrowser::FExtensionHost& Host, bool bAllowAssetMutation) {
				Integration->Presentation.Draw(Host, [&] { if (Integration->ImportDialog) Integration->ImportDialog->Draw(bAllowAssetMutation); },
					[&] { Integration->ImportDialog.reset(); });
			},
			.StopHostPresentation = [this](const std::weak_ptr<void>& Lifetime) {
				Integration->Presentation.Stop(Lifetime, [&] { Integration->ImportDialog.reset(); });
			},
			}, Error);
		if (!ImportExtension.IsValid())
		{
			DURIN_ERROR("Could not register Content Browser Static Mesh import: {}", Error);
			UnregisterContributions();
			return false;
		}
		Integration->ImportExtension = std::move(ImportExtension);
		std::string PresentationError;

		return true;
	}

	auto FStaticMeshEditorModule::UnregisterContributions() -> void
	{
		Integration->ImportExtension.Reset();
		Integration->ImportDialog.reset();
		ThumbnailRegistration.reset();
	}

}
