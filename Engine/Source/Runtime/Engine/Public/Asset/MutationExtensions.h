#pragma once

#include "Asset/AssetDefinitions.h"

#include "EngineAPI.h"
#include "Asset/Relocation.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DClass;
	class DObject;
}

namespace Durin
{
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

	// Returns an error if any provider cannot be inspected. Partial output must not be used.
	ENGINE_API auto CaptureAssetReferenceStores(FAssetReferenceStoreCapture& OutCapture)
		-> FAssetResult;

	struct FAssetReferenceStorePackageRewrite
	{
		FPackagePath PackagePath;
		FByteArray PreBytes;
		FByteArray PostBytes;
	};

	struct FAssetReferenceStoreRewriteContribution
	{
		std::string Fingerprint;
		std::vector<FAssetReferenceRewrite> Rewrites;
		std::vector<FAssetReferenceStorePackageRewrite> PackageRewrites;
		std::function<FAssetResult()> Revalidate;
		std::function<FAssetResult()> Apply;
		std::function<FAssetResult()> Restore;
		std::function<FAssetResult()> Verify;
	};

	class IAssetReferenceStore
	{
	public:
		virtual ~IAssetReferenceStore() = default;
		virtual auto CaptureSnapshot(FAssetReferenceStoreSnapshot& OutSnapshot)
			-> FAssetResult = 0;
		virtual auto PrepareRewrite(
			std::span<const FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			FAssetReferenceStoreRewriteContribution& OutContribution
		) -> FAssetResult = 0;
	};

	using FAssetReferenceStoreHandle = uint64;
	ENGINE_API auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store,
		FModuleOwnedCallbackGate OwnerGate = {}
	) -> FAssetReferenceStoreHandle;
	ENGINE_API auto UnregisterAssetReferenceStore(
		FAssetReferenceStoreHandle Handle
	) -> void;

	struct FAssetOwnedPayloadRelocation
	{
		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Files;
		std::function<void()> Apply;
		std::function<void()> Restore;
	};

	using FAssetOwnedPayloadRelocator = std::function<FAssetResult(
		DObject*,
		const FPackagePath&,
		const FPackagePath&,
		FAssetOwnedPayloadRelocation&
	)>;
	using FAssetOwnedPayloadRelocatorHandle = uint64;
	ENGINE_API auto RegisterAssetOwnedPayloadRelocator(
		DClass* Class,
		FAssetOwnedPayloadRelocator Relocator,
		FModuleOwnedCallbackGate OwnerGate = {}
	) -> FAssetOwnedPayloadRelocatorHandle;
	ENGINE_API auto UnregisterAssetOwnedPayloadRelocator(
		FAssetOwnedPayloadRelocatorHandle Handle
	) -> void;

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
		IAssetMoveObserver* Observer,
		FModuleOwnedCallbackGate OwnerGate = {}
	) -> FAssetMoveObserverHandle;
	ENGINE_API auto UnregisterAssetMoveObserver(
		FAssetMoveObserverHandle Handle
	) -> void;
} // namespace Durin
