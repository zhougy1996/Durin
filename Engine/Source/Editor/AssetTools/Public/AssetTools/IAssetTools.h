#pragma once

#include "CoreMinimal.h"

#include "AssetToolsAPI.h"
#include "AssetTools/AssetDeletion.h"
#include "Misc/FeedbackContext.h"
#include "AssetTools/AssetDuplicate.h"
#include "AssetTools/AssetImport.h"
#include "AssetTools/AssetMutation.h"
#include "AssetTools/AssetSave.h"
#include "Asset/PackageReload.h"
#include "DObject/Object.h"

namespace Durin
{
	class DClass;
	class DFactory;
	class DPackage;

	// Coordinates generic editor asset construction and delegates specialized
	// initialization to factories when one is available. Returned assets are live
	// and unsaved; Standalone package residency keeps them alive.
	class IAssetTools
	{
	public:
		virtual ~IAssetTools() = default;
		ASSETTOOLS_API static auto Get() -> IAssetTools&;

		virtual auto CreateAsset(
			const FTopLevelAssetPath& AssetPath,
			DClass* AssetClass,
			const DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = EObjectFlags::Public | EObjectFlags::Transactional,
			FFeedbackContext* Warn = nullptr)
			-> DObject* = 0;

		// Input order; null entries denote rejected, canceled or unattempted imports.
		ASSETTOOLS_API auto ImportAssets(const FAssetImportBatchRequest& Request,
			FFeedbackContext* Warn = nullptr) -> std::vector<DObject*>;

		virtual auto ImportAsset(
			const FTopLevelAssetPath& AssetPath,
			DClass* AssetClass,
			std::string_view Filename,
			const DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = EObjectFlags::Public | EObjectFlags::Transactional,
			FFeedbackContext* Warn = nullptr)
			-> DObject* = 0;

		auto CreatePackageLeafAssetForTesting(
			const FPackagePath& PackagePath,
			DClass* AssetClass,
			const DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = EObjectFlags::Public | EObjectFlags::Transactional,
			FFeedbackContext* Warn = nullptr) -> DObject*
		{
			FTopLevelAssetPath AssetPath;
			if (!FTopLevelAssetPath::TryCreate(
				PackagePath, PackagePath.GetPackageName(), AssetPath)) return {};
			return CreateAsset(AssetPath, AssetClass, Factory, Context, Flags, Warn);
		}

		auto ImportPackageLeafAssetForTesting(
			const FPackagePath& PackagePath,
			DClass* AssetClass,
			std::string_view Filename,
			const DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = EObjectFlags::Public | EObjectFlags::Transactional,
			FFeedbackContext* Warn = nullptr) -> DObject*
		{
			FTopLevelAssetPath AssetPath;
			if (!FTopLevelAssetPath::TryCreate(
				PackagePath, PackagePath.GetPackageName(), AssetPath)) return {};
			return ImportAsset(
				AssetPath, AssetClass, Filename, Factory, Context, Flags, Warn);
		}

		// Discards a package created through this service. Unsaved state is
		// intentionally abandoned and the full object hierarchy is collected.
		virtual auto DiscardPackage(DPackage* Package) -> bool = 0;

		// Restores the bounded package set from its current saved disk closure.
		// Unlike DiscardPackage, this preserves residency and live references.
		virtual auto RecoverPackages(const FPackageReloadRequest& Request)
			-> FPackageReloadOperation = 0;

		// Chooses a collision-free copy identity, clones the persistent graph, and
		// returns the new Dirty asset. Saving is a separate caller action.
		virtual auto DuplicateAsset(const FAssetDuplicateRequest& Request, FFeedbackContext* Warn = nullptr)
			-> DObject* = 0;
		// Persists loaded dirty packages or executes canonical resave policy.
		virtual auto SaveAssets(const FAssetSaveRequest& Request, FFeedbackContext* Warn = nullptr)
			-> bool = 0;
		// Executes one authored relocation job once.
		virtual auto RelocateAssets(const FAssetRelocationRequest& Request, FFeedbackContext* Warn = nullptr)
			-> bool = 0;
		// Executes one redirect-reference canonicalization job.
		virtual auto FixUpRedirectors(
			const FAssetRedirectorFixupRequest& Request, FFeedbackContext* Warn = nullptr)
			-> bool = 0;
		// Captures asset safety and the opaque irreversible operation without staging bytes.
		virtual auto PrepareDeletion(
			const FAssetDeletionRequest& Request,
			FAssetDeletionOperation& OutOperation, FFeedbackContext* Warn = nullptr)
			-> bool = 0;
	};

}
