#pragma once

#include "Asset/AssetWriteResult.h"
#include "Asset/AssetReadResult.h"

#include "EngineAPI.h"
#include "AssetRegistry/Catalog.h"

namespace Durin
{
	class DClass;
	class DObject;
}

namespace Durin
{
	struct FAssetRelocationMapping
	{
		FPackagePath SourcePath;
		FPackagePath DestinationPath;

		auto operator==(const FAssetRelocationMapping&) const -> bool = default;
	};

	struct FAssetReferenceStoreOccurrence
	{
		std::string ProviderId;
		std::string StableId;
		FPackagePath TargetPath;
		std::string DisplayRoute;
		std::string ExpectedClass;
		bool bCookRoot = false;

		auto operator==(const FAssetReferenceStoreOccurrence&) const -> bool = default;
	};

	struct FAssetReferenceRewrite
	{
		std::string StableId;
		FPackagePath SourcePath;
		FPackagePath DestinationPath;

		auto operator==(const FAssetReferenceRewrite&) const -> bool = default;
	};

	struct FAssetReferenceStoreSnapshot
	{
		std::string ProviderId;
		uint64 ProviderVersion = 0;
		std::string Fingerprint;
		std::vector<FAssetReferenceStoreOccurrence> Occurrences;

		auto operator==(const FAssetReferenceStoreSnapshot&) const -> bool = default;
	};

	// Owned provider facts captured under their module gates, shared by authoring tools.
	struct FAssetReferenceStoreCapture
	{
		uint64 RegistryRevision = 0;
		std::vector<FAssetReferenceStoreSnapshot> Stores;

		auto operator==(const FAssetReferenceStoreCapture&) const -> bool = default;
	};

	// Owner-thread operation. Providers and their code must survive their capture
	// callbacks. Registration changes during callbacks fail with StaleData and
	// empty output; successful output owns its values and retains no providers.
	ENGINE_API auto CaptureAssetReferenceStores(FAssetReferenceStoreCapture& OutCapture)
		-> FAssetReadResult;

	struct FAssetReferenceStorePackageRewrite
	{
		FPackagePath PackagePath;
		FByteBuffer PreBytes;
		FByteBuffer PostBytes;
	};

	struct FAssetReferenceStoreRewriteContribution
	{
		std::string Fingerprint;
		std::vector<FAssetReferenceRewrite> Rewrites;
		std::vector<FAssetReferenceStorePackageRewrite> PackageRewrites;
		std::function<FAssetWriteResult()> Revalidate;
		std::function<FAssetWriteResult()> Apply;
		std::function<FAssetWriteResult()> Restore;
		std::function<FAssetWriteResult()> Verify;
	};

	class IAssetReferenceStore
	{
	public:
		virtual ~IAssetReferenceStore() = default;
		virtual auto CaptureSnapshot(FAssetReferenceStoreSnapshot& OutSnapshot)
			-> FAssetReadResult = 0;
		virtual auto PrepareRewrite(
			std::span<const FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			FAssetReferenceStoreRewriteContribution& OutContribution
		) -> FAssetWriteResult = 0;
	};

	using FAssetReferenceStoreHandle = uint64;
	struct FAssetReferenceStoreRegistrations
	{
		uint64 Revision = 0;
		std::map<FAssetReferenceStoreHandle, IAssetReferenceStore*> Stores;
	};
	// Borrowed providers: verify the revision after callbacks and before reuse.
	// The caller must keep provider objects/code alive for the operation.
	ENGINE_API auto CaptureAssetReferenceStoreRegistrations() -> FAssetReferenceStoreRegistrations;
	ENGINE_API auto GetAssetReferenceStoreRevision() -> uint64;

	// Owner-thread registration. Unregister and finish active capture/mutation/
	// fix-up callbacks before destroying the store or unloading provider code.
	ENGINE_API auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store) -> FAssetReferenceStoreHandle;
	// Pins the store and its provider code while collecting owned root snapshots.
	// Mutation/fix-up callbacks retain their existing caller lifetime contract.
	ENGINE_API auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store, std::shared_ptr<void> CaptureOwner)
		-> FAssetReferenceStoreHandle;

	ENGINE_API auto UnregisterAssetReferenceStore(
		FAssetReferenceStoreHandle Handle
	) -> void;

	struct FAssetOwnedPayloadRelocation
	{
		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Files;
		std::function<void()> Apply;
		std::function<void()> Restore;
	};

	using FAssetOwnedPayloadRelocator = std::function<FAssetWriteResult(
		DObject*,
		const FPackagePath&,
		const FPackagePath&,
		FAssetOwnedPayloadRelocation&
	)>;
	using FAssetOwnedPayloadRelocatorHandle = uint64;
	ENGINE_API auto RegisterAssetOwnedPayloadRelocator(
		DClass* Class,
		FAssetOwnedPayloadRelocator Relocator) -> FAssetOwnedPayloadRelocatorHandle;

	ENGINE_API auto UnregisterAssetOwnedPayloadRelocator(
		FAssetOwnedPayloadRelocatorHandle Handle
	) -> void;

	// Copies the extension for this class or its nearest registered superclass.
	// Release the callback before unloading its provider code.
	ENGINE_API auto FindAssetOwnedPayloadRelocator(DClass* AssetClass)
		-> FAssetOwnedPayloadRelocator;

	class IAssetMoveObserver
	{
	public:
		virtual ~IAssetMoveObserver() = default;
		virtual auto OnAssetsRelocated(
			std::span<const FAssetRelocationMapping> Mappings
		) -> void = 0;
	};

	using FAssetMoveObserverHandle = uint64;
	ENGINE_API auto RegisterAssetMoveObserver(
		IAssetMoveObserver* Observer) -> FAssetMoveObserverHandle;

	ENGINE_API auto UnregisterAssetMoveObserver(
		FAssetMoveObserverHandle Handle
	) -> void;
	ENGINE_API auto NotifyAssetMoveObservers(
		std::span<const FAssetRelocationMapping> Mappings) -> void;
} // namespace Durin
