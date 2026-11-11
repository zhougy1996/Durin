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
					OwnerLifetime = Invocation.bHasHostLifetime ? std::optional(Invocation.HostLifetime) : std::nullopt;
					ImportDialog = std::make_unique<FGltfAssetImportDialog>(ContentBrowser::MakeImportDialogCallbacks(Invocation, {}));
					ImportDialog->Open(Invocation.Context.VirtualDirectory);
				},
				.DrawHostPresentation = [this](const ContentBrowser::FExtensionHost& Host, bool bAllowed) {
					if (OwnerLifetime && OwnerLifetime->expired()) { ImportDialog.reset(); OwnerLifetime.reset(); return; }
					if (OwnerLifetime && (OwnerLifetime->owner_before(Host.Lifetime) || Host.Lifetime.owner_before(*OwnerLifetime))) return;
					if (ImportDialog) ImportDialog->Draw(bAllowed);
				},
				.StopHostPresentation = [this](const std::weak_ptr<void>& Lifetime) {
					if (OwnerLifetime && !OwnerLifetime->owner_before(Lifetime) && !Lifetime.owner_before(*OwnerLifetime)) { ImportDialog.reset(); OwnerLifetime.reset(); }
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
		std::optional<std::weak_ptr<void>> OwnerLifetime;
		std::unique_ptr<FGltfAssetImportDialog> ImportDialog;
		ContentBrowser::FScopedExtensionRegistration ImportExtension;
	};

	IMPLEMENT_MODULE(FAssetForgeBuiltinsModule, AssetForgeBuiltins)
}
