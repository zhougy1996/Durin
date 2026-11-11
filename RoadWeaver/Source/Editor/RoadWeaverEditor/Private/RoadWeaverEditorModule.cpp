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
					OwnerLifetime = Invocation.bHasHostLifetime ? std::optional(Invocation.HostLifetime) : std::nullopt;
					CreateDialog = std::make_unique<FRoadNetCreateDialog>();
					CreateDialog->Open(Invocation.Context.VirtualDirectory, {
						.NotifyMountedContentChanged =
							Invocation.NotifyMountedContentChanged,
						.RevealAsset = Invocation.RevealAsset,
						.ReportError = Invocation.ReportError});
				},
				.DrawHostPresentation = [this](const ::Durin::ContentBrowser::FExtensionHost& Host, bool bAllowAssetMutation) {
					if (OwnerLifetime && OwnerLifetime->expired()) { CreateDialog.reset(); OwnerLifetime.reset(); return; }
					if (OwnerLifetime && (OwnerLifetime->owner_before(Host.Lifetime) || Host.Lifetime.owner_before(*OwnerLifetime))) return;
					if (CreateDialog) CreateDialog->Draw(bAllowAssetMutation);
				},
				.StopHostPresentation = [this](const std::weak_ptr<void>& Lifetime) {
					if (OwnerLifetime && !OwnerLifetime->owner_before(Lifetime) && !Lifetime.owner_before(*OwnerLifetime))
					{ CreateDialog.reset(); OwnerLifetime.reset(); }
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
		std::optional<std::weak_ptr<void>> OwnerLifetime;
	};

	IMPLEMENT_MODULE(FRoadWeaverEditorModule, RoadWeaverEditor)
} // namespace Durin::RoadNet::Editor
