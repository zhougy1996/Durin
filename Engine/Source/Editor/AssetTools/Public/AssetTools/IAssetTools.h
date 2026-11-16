#pragma once

#include "CoreMinimal.h"

#include "AssetToolsAPI.h"
#include "AssetTools/AssetDeletion.h"
#include "Misc/FeedbackContext.h"
#include "AssetTools/AssetImport.h"
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

		// Input order; null entries denote rejected, canceled or unattempted imports.
		ASSETTOOLS_API auto ImportAssets(const FAssetImportBatchRequest& Request,
			FFeedbackContext* Warn = nullptr) -> std::vector<DObject*>;

		virtual auto ImportAsset(
			const FTopLevelAssetPath& AssetPath,
			DClass* AssetClass,
			std::string_view Filename,
			DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = (EObjectFlags::Public | EObjectFlags::Standalone | EObjectFlags::Transactional),
			FFeedbackContext* Warn = nullptr)
			-> DObject* = 0;

		auto ImportPackageLeafAssetForTesting(
			const FPackagePath& PackagePath,
			DClass* AssetClass,
			std::string_view Filename,
			DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = (EObjectFlags::Public | EObjectFlags::Standalone | EObjectFlags::Transactional),
			FFeedbackContext* Warn = nullptr) -> DObject*
		{
			FTopLevelAssetPath AssetPath;
			if (!FTopLevelAssetPath::TryCreate(
				PackagePath, PackagePath.GetPackageName(), AssetPath)) return {};
			return ImportAsset(
				AssetPath, AssetClass, Filename, Factory, Context, Flags, Warn);
		}

		virtual auto RenameAssets(std::span<const FAssetRenameData> AssetsAndNames) -> bool = 0;
		virtual auto OnAssetPostRename() -> TMulticastDelegate<void(std::span<const FAssetRenameData>)>& = 0;
		virtual auto FixupReferencers(std::span<DAssetRedirector* const> Objects, bool bCheckoutDialogPrompt = true,
			ERedirectFixupMode Mode = ERedirectFixupMode::DeleteFixedUpRedirectors) const -> void = 0;
		virtual auto IsFixupReferencersInProgress() const -> bool = 0;
		// Captures asset safety and the opaque irreversible operation without staging bytes.
		virtual auto PrepareDeletion(
			const FAssetDeletionRequest& Request,
			FAssetDeletionOperation& OutOperation, FFeedbackContext* Warn = nullptr)
			-> bool = 0;
	};

}
