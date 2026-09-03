#include "Modules/ModuleManager.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Dialogs/RoadNetCreateDialog.h"

namespace Durin::RoadNet::Editor
{
	// Registers RoadWeaver authoring integrations without entering game builds.
	class FRoadWeaverEditorModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			CreateDialog = std::make_unique<FRoadNetCreateDialog>();
			std::string Error;
			CreateExtension = ::Durin::Editor::ContentBrowser::RegisterExtension({
				.Id = "roadweaver.create-road-net",
				.Label = "Road Net...",
				.Category = ::Durin::Editor::ContentBrowser::EExtensionCategory::Create,
				.Order = 300,
				.IsApplicable = [](const auto& Context) {
					return !Context.VirtualDirectory.empty();
				},
				.Invoke = [this](const auto& Invocation) {
					if (!CreateDialog) return;
					CreateDialog->Open(Invocation.Context.VirtualDirectory, {
						.NotifyMountedContentChanged =
							Invocation.NotifyMountedContentChanged,
						.RevealAsset = Invocation.RevealAsset,
						.ReportError = Invocation.ReportError});
				},
				.DrawHostPresentation = [this](bool bAllowAssetMutation) {
					if (CreateDialog) CreateDialog->Draw(bAllowAssetMutation);
				},
			}, Error);
			if (!CreateExtension.IsValid())
				DURIN_ERROR("Could not register Road Net creation: {}", Error);
		}

		auto ShutdownModule() -> void override
		{
			CreateExtension.Reset();
			CreateDialog.reset();
		}

	private:
		::Durin::Editor::ContentBrowser::FScopedExtensionRegistration CreateExtension;
		std::unique_ptr<FRoadNetCreateDialog> CreateDialog;
	};

	IMPLEMENT_MODULE(FRoadWeaverEditorModule, RoadWeaverEditor)
} // namespace Durin::RoadNet::Editor
