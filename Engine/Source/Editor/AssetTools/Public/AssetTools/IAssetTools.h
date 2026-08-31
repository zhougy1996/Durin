#pragma once

#include "AssetToolsAPI.h"
#include "AssetTools/AssetDeletion.h"
#include "AssetTools/AssetDuplicate.h"
#include "AssetTools/AssetMutation.h"
#include "AssetTools/AssetSave.h"
#include "DObject/Object.h"

namespace Durin
{
	class DClass;
	class DFactory;
	class DPackage;

	using FAssetToolsResult = FAssetOperationResult;

	// Coordinates editor asset construction while factories own object-specific
	// initialization. Returned assets are live and unsaved; the package is kept
	// alive by Standalone residency rather than the permanent root set.
	class IAssetTools
	{
	public:
		virtual ~IAssetTools() = default;
		ASSETTOOLS_API static auto Get() -> IAssetTools&;

		virtual auto CreateAsset(
			const FPackagePath& AssetPath,
			DClass* AssetClass,
			const DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = EObjectFlags::Public)
			-> FAssetToolsResult = 0;

		virtual auto ImportAsset(
			const FPackagePath& AssetPath,
			DClass* AssetClass,
			std::string_view Filename,
			const DFactory* Factory = nullptr,
			DObject* Context = nullptr,
			EObjectFlags Flags = EObjectFlags::Public)
			-> FAssetToolsResult = 0;

		// Discards a package created through this service. Unsaved state is
		// intentionally abandoned and the full object hierarchy is collected.
		virtual auto DiscardPackage(DPackage* Package) -> bool = 0;

		// Chooses a collision-free copy identity, clones the persistent graph, and
		// applies the request's dirty-versus-persisted publication policy.
		virtual auto DuplicateAsset(const FAssetDuplicateRequest& Request)
			-> FAssetOperationResult = 0;
		// Persists loaded dirty packages or executes canonical resave policy.
		virtual auto SaveAssets(const FAssetSaveRequest& Request)
			-> FAssetOperationResult = 0;
		// Commits one opaque relocation transaction into global editor history.
		virtual auto RelocateAssets(const FAssetRelocationRequest& Request)
			-> FAssetOperationResult = 0;
		// Rewrites redirect references through one reversible Engine transaction.
		virtual auto FixUpRedirectors(
			const FAssetRedirectorFixupRequest& Request)
			-> FAssetOperationResult = 0;
		// Captures asset safety and the opaque transaction without staging bytes.
		virtual auto PrepareDeletion(
			const FAssetDeletionRequest& Request,
			FAssetDeletionOperation& OutOperation)
			-> FAssetOperationResult = 0;
	};

}
