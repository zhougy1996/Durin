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
			CreateExtension = ::Durin::ContentBrowser::RegisterExtension({
				.Id = "roadweaver.create-road-net",
				.Label = "Road Net...",
				.Category = ::Durin::ContentBrowser::EExtensionCategory::Create,
				.Order = 300,
				.Mutation = ::Durin::ContentBrowser::EContentMutation::MutatesContent,
				.IsApplicable = [](const auto& Context) {
					return !Context.VirtualDirectory.empty();
				},
				.Invoke = [this](const auto& Invocation) {
					Presentation.Bind(Invocation.Host);
					CreateDialog = std::make_unique<FRoadNetCreateDialog>();
					CreateDialog->Open(Invocation.Context.VirtualDirectory, {
						.NotifyMountedContentChanged =
							Invocation.NotifyMountedContentChanged,
						.RevealAsset = Invocation.RevealAsset,
						.ReportError = Invocation.ReportError});
				},
				.DrawHostPresentation = [this](const ::Durin::ContentBrowser::FExtensionHost& Host, bool bAllowAssetMutation) {
					Presentation.Draw(Host, [&] { if (CreateDialog) CreateDialog->Draw(bAllowAssetMutation); },
						[&] { CreateDialog.reset(); });
				},
				.StopHostPresentation = [this](const std::weak_ptr<void>& Lifetime) {
					Presentation.Stop(Lifetime, [&] { CreateDialog.reset(); });
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
		::Durin::ContentBrowser::FScopedExtensionRegistration CreateExtension;
		std::unique_ptr<FRoadNetCreateDialog> CreateDialog;
		ContentBrowser::FScopedHostPresentation Presentation;
	};

	IMPLEMENT_MODULE(FRoadWeaverEditorModule, RoadWeaverEditor)
} // namespace Durin::RoadNet::Editor
