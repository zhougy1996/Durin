#include "Modules/ModuleManager.h"
#include "AssetForgeBuiltinsAssetFeatures.h"
#include "GltfAssetImportDialog.h"
#include "ContentBrowser/ContentBrowserContracts.h"

namespace Durin
{
	class FAssetForgeBuiltinsModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			SaveReadinessRegistration = FModuleStartup::RegisterFeature<
				IAssetSaveReadinessFeature>(AssetFeatures);
			require(SaveReadinessRegistration.IsValid());
			ImportDialog = std::make_unique<Editor::FGltfAssetImportDialog>();
			std::string Error;
			ImportExtension = Editor::ContentBrowser::RegisterExtension({
				.Id = "gltf.import-assets", .Label = "glTF / GLB Assets...",
				.Category = Editor::ContentBrowser::EExtensionCategory::Import, .Order = 310,
				.Mutation = Editor::ContentBrowser::EContentMutation::MutatesContent,
				.IsApplicable = [](const auto& Context) { return !Context.VirtualDirectory.empty(); },
				.Invoke = [this](const auto& Invocation) {
					ImportDialog->SetCallbacks({
						.ReportError = Invocation.ReportError,
						.AssetCreated = [Reveal = Invocation.RevealAsset](std::string Path) { if (Reveal) (void)Reveal(Path); },
						.ImportedDirectory = [Changed = Invocation.NotifyMountedContentChanged, Reveal = Invocation.RevealDirectory](std::string Path) {
							if (Changed) Changed();
							if (Reveal) (void)Reveal(Path);
						}});
					ImportDialog->Open(Invocation.Context.VirtualDirectory);
				},
				.DrawHostPresentation = [this](bool bAllowed) { ImportDialog->Draw(bAllowed); }}, Error);
			require(ImportExtension.IsValid());

		}

		auto ShutdownModule() -> void override
		{
			ImportExtension.Reset();
			ImportDialog.reset();
			if (SaveReadinessRegistration.IsValid())
				require(SaveReadinessRegistration.Reset() == EModularFeatureRetirementStatus::Succeeded);
		}

	private:
		AssetForge::Builtins::FAssetForgeBuiltinsAssetFeatures AssetFeatures;
		FModularFeatureRegistration SaveReadinessRegistration;
		std::unique_ptr<Editor::FGltfAssetImportDialog> ImportDialog;
		Editor::ContentBrowser::FScopedExtensionRegistration ImportExtension;
	};

	IMPLEMENT_MODULE(FAssetForgeBuiltinsModule, AssetForgeBuiltins)
}
