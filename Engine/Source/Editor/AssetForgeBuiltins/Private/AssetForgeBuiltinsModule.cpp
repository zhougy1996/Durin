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
			SaveReadinessRegistration = FModularFeatureRegistry::Get().RegisterFeature<
				IAssetSaveReadinessFeature>(AssetFeatures);
			require(SaveReadinessRegistration.IsValid());
			std::string Error;
			ImportExtension = ContentBrowser::RegisterExtension({
				.Id = "gltf.import-assets", .Label = "glTF / GLB Assets...",
				.Category = ContentBrowser::EExtensionCategory::Import, .Order = 310,
				.Mutation = ContentBrowser::EContentMutation::MutatesContent,
				.IsApplicable = [](const auto& Context) { return !Context.VirtualDirectory.empty(); },
				.Invoke = [this](const auto& Invocation) {
					Presentation.Bind(Invocation.Host);
					ImportDialog = std::make_unique<FGltfAssetImportDialog>(ContentBrowser::MakeImportDialogCallbacks(Invocation, {}));
					ImportDialog->Open(Invocation.Context.VirtualDirectory);
				},
				.DrawHostPresentation = [this](const ContentBrowser::FExtensionHost& Host, bool bAllowed) {
					Presentation.Draw(Host, [&] { if (ImportDialog) ImportDialog->Draw(bAllowed); },
						[&] { ImportDialog.reset(); });
				},
				.StopHostPresentation = [this](const std::weak_ptr<void>& Lifetime) {
					Presentation.Stop(Lifetime, [&] { ImportDialog.reset(); });
				}}, Error);
			require(ImportExtension.IsValid());

		}

		auto ShutdownModule() -> void override
		{
			ImportExtension.Reset();
			ImportDialog.reset();
			SaveReadinessRegistration.Reset();
		}

	private:
		AssetForge::Builtins::FAssetForgeBuiltinsAssetFeatures AssetFeatures;
		FModularFeatureRegistration SaveReadinessRegistration;
		ContentBrowser::FScopedHostPresentation Presentation;
		std::unique_ptr<FGltfAssetImportDialog> ImportDialog;
		ContentBrowser::FScopedExtensionRegistration ImportExtension;
	};

	IMPLEMENT_MODULE(FAssetForgeBuiltinsModule, AssetForgeBuiltins)
}
