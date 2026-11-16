#pragma once

#include "CoreMinimal.h"

#include "AssetToolsAPI.h"
#include "AssetTools/AssetDeleteExtensions.h"
#include "Misc/FeedbackContext.h"
#include "AssetRenameData.h"
#include "DObject/Object.h"

namespace Durin
{
	class DClass;
	class DFactory;
	class DPackage;
	class DAssetRedirector;
	class DAssetImportTask;

	// Coordinates generic editor asset construction and delegates specialized
	// initialization to factories when one is available. Returned assets are live
	// and unsaved; the default Standalone flag keeps the authored object available for editing.
	class IAssetTools
	{
	public:
		virtual ~IAssetTools() = default;
		ASSETTOOLS_API static auto Get() -> IAssetTools&;

		// Creates a live Dirty asset. PackagePath is its directory; replacement preserves the old graph on rejection.
		virtual auto CreateAsset(std::string_view AssetName, std::string_view PackagePath,
			DClass* AssetClass, DFactory* Factory, FName CallingContext = {}, bool bOverwriteExisting = false) -> DObject* = 0;
		// Explicit target identity. Naming and saving are separate caller decisions.
		virtual auto DuplicateAsset(std::string_view AssetName, std::string_view PackagePath, DObject* OriginalObject) -> DObject* = 0;
		virtual auto CreateUniqueAssetName(std::string_view BasePackageName, std::string_view Suffix,
			std::string& OutPackageName, std::string& OutAssetName) -> void = 0;

		virtual auto ImportAssetTasks(std::span<DAssetImportTask* const> Tasks) -> void = 0;

		virtual auto ImportAssets(std::span<const std::string> Files, std::string_view DestinationPath,
			DFactory* ChosenFactory = nullptr, bool bSyncToBrowser = true,
			std::vector<std::pair<std::string, std::string>>* FilesAndDestinations = nullptr,
			bool bAllowAsyncImport = false, bool bSceneImport = false) const -> std::vector<DObject*> = 0;

		virtual auto RenameAssets(std::span<const FAssetRenameData> AssetsAndNames) -> bool = 0;
		virtual auto OnAssetPostRename() -> TMulticastDelegate<void(std::span<const FAssetRenameData>)>& = 0;
		virtual auto FixupReferencers(std::span<DAssetRedirector* const> Objects, bool bCheckoutDialogPrompt = true,
			ERedirectFixupMode Mode = ERedirectFixupMode::DeleteFixedUpRedirectors) const -> void = 0;
		virtual auto IsFixupReferencersInProgress() const -> bool = 0;
	};

}
