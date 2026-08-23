#pragma once

#include "Asset/Relocation.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DClass;
	class DObject;
}

namespace Durin::Asset
{
	struct FAssetReferenceStoreOccurrence
	{
		std::string ProviderId;
		std::string StableId;
		FAssetPath TargetPath;
		std::string DisplayRoute;
		std::string ExpectedClass;
		bool bCookRoot = false;

		auto operator==(const FAssetReferenceStoreOccurrence&) const -> bool = default;
	};

	struct FAssetReferenceRewrite
	{
		std::string StableId;
		FAssetPath SourcePath;
		FAssetPath DestinationPath;

		auto operator==(const FAssetReferenceRewrite&) const -> bool = default;
	};

	struct FAssetReferenceStoreSnapshot
	{
		std::string ProviderId;
		uint64 ProviderVersion = 0;
		std::string Fingerprint;
		std::vector<FAssetReferenceStoreOccurrence> Occurrences;
	};

	struct FAssetReferenceStorePackageRewrite
	{
		FAssetPath PackagePath;
		std::vector<std::byte> PreBytes;
		std::vector<std::byte> PostBytes;
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
	ASSETCORE_API auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store,
		FModuleOwnedCallbackGate OwnerGate = {}
	) -> FAssetReferenceStoreHandle;
	ASSETCORE_API auto UnregisterAssetReferenceStore(
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
		const FAssetPath&,
		const FAssetPath&,
		FAssetOwnedPayloadRelocation&
	)>;
	using FAssetOwnedPayloadRelocatorHandle = uint64;
	ASSETCORE_API auto RegisterAssetOwnedPayloadRelocator(
		DClass* Class,
		FAssetOwnedPayloadRelocator Relocator,
		FModuleOwnedCallbackGate OwnerGate = {}
	) -> FAssetOwnedPayloadRelocatorHandle;
	ASSETCORE_API auto UnregisterAssetOwnedPayloadRelocator(
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
	ASSETCORE_API auto RegisterAssetMoveObserver(
		IAssetMoveObserver* Observer,
		FModuleOwnedCallbackGate OwnerGate = {}
	) -> FAssetMoveObserverHandle;
	ASSETCORE_API auto UnregisterAssetMoveObserver(
		FAssetMoveObserverHandle Handle
	) -> void;
} // namespace Durin::Asset
