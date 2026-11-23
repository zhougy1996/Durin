#include "CoreMinimal.h"

#include "Asset/PackageSerialization.h"
#include "AssetTools/IAssetTools.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "DObject/Class.h"
#include "Logging/LogMacros.h"
#include "Misc/FeedbackContext.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "WaterWaves.h"

namespace Durin
{
	namespace
	{
		auto CreateWaterWaves(const FTopLevelAssetPath& AssetPath, std::string& Error) -> bool
		{
			FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
				if (Level < ELogLevel::Error) return;
				if (!Error.empty()) Error += "\n";
				Error += Message;
			});
			FScopedFeedbackContext FeedbackScope(Feedback);
			const auto PackagePath = AssetPath.GetPackagePath().ToString();
			const auto Directory = PackagePath.substr(0, PackagePath.find_last_of('/'));
			auto* Asset = IAssetTools::Get().CreateAsset(AssetPath.GetAssetName(), Directory, DWaterWaves::StaticClass(), nullptr);
			if (!Asset)
			{
				if (Error.empty()) Error = "Could not create the Water Waves asset.";
				return false;
			}

			FAssetWriteResult Result;
			if (SavePackage(Asset->GetPackage(), {}, &Result)) return true;
			Error = Result.Error ? Result.Error->ToString() : "Could not save the Water Waves asset.";
			std::string UnloadError;
			if (!DPackageTools::UnloadPackages(std::array{Asset->GetPackage()}, UnloadError, true) || !UnloadError.empty())
				Error += " The unsaved Water Waves package could not be discarded.";
			return false;
		}
	}

	class FWaterEditorModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			std::string Error;
			Creation = ContentBrowser::RegisterAssetCreation({
				.Id = "water.create-waves",
				.Label = "Water Waves",
				.DefaultName = "NewWaterWaves",
				.Order = 250,
				.Create = CreateWaterWaves,
				.AssetClassNameToOpen = DWaterWaves::StaticClass()->GetQualifiedName().ToString(),
			}, Error);
			if (!Creation.IsValid()) DURIN_ERROR("Could not register Water Waves asset creation: {}", Error);
		}

		auto ShutdownModule() -> void override { Creation.Reset(); }

	private:
		ContentBrowser::FScopedExtensionRegistration Creation;
	};

	IMPLEMENT_MODULE(FWaterEditorModule, WaterEditor)
}
