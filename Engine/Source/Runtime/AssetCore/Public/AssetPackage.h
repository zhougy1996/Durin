#pragma once

#include "Modules/ModularFeature.h"

#include "AssetCoreAPI.h"
#include "AssetCatalog.h"
#include "AssetResult.h"
#include "CookedAsset.h"
#include "Delegates/Delegate.h"
#include "DObject/CoreDObject.h"
#include "Hash/XxHash.h"

namespace Durin::Asset
{
	class DAssetRedirector;

	enum class ESoftObjectNullPolicy : uint8
	{
		Reject,
		Allow
	};

	enum class ESoftObjectResolveState : uint8
	{
		Null,
		NotLoaded,
		Loaded
	};

	struct FSoftObjectResolveResult
	{
		FAssetResult Result;
		ESoftObjectResolveState State = ESoftObjectResolveState::Null;
		DObject* Object = nullptr;
		FAssetPath ResolvedPath;
		bool bRedirected = false;

		auto Succeeded() const -> bool { return Result.Succeeded(); }
		explicit operator bool() const { return Succeeded(); }
	};

	template<typename T>
	struct TSoftObjectResolveResult
	{
		FAssetResult Result;
		ESoftObjectResolveState State = ESoftObjectResolveState::Null;
		T* Object = nullptr;
		FAssetPath ResolvedPath;
		bool bRedirected = false;

		auto Succeeded() const -> bool { return Result.Succeeded(); }
		explicit operator bool() const { return Succeeded(); }
	};

	enum class EAssetLoadMutationKind : uint8
	{
		Upgrade,
		NonUpgrade
	};

	// Records an authored-state change made while materializing a package.
	struct FAssetLoadMutation
	{
		FAssetPath PackagePath;
		std::string ObjectPath;
		std::string HandlerId;
		std::string Summary;
		EAssetLoadMutationKind Kind = EAssetLoadMutationKind::NonUpgrade;
	};

	// Stable string names for these values are part of canonical-resave reports.
	enum class EAssetReflectedIdentityKind : uint8 { Class, Struct, Enum };
	enum class EAssetSerializedIdentityLocation : uint8
	{
		PackageHeader,
		ObjectRecord,
		Schema,
		TypeDescriptor
	};

	struct FAssetCanonicalizationEvidence
	{
		FAssetPath PackagePath;
		std::string StoredIdentity;
		std::string CurrentIdentity;
		EAssetReflectedIdentityKind Kind = EAssetReflectedIdentityKind::Class;
		EAssetSerializedIdentityLocation Location = EAssetSerializedIdentityLocation::PackageHeader;
		std::string LogicalPath;

		auto operator==(const FAssetCanonicalizationEvidence&) const -> bool = default;
	};

	// Carries structured compatibility results for one loaded package.
	struct FAssetLoadReport
	{
		FAssetPath RequestedPath;
		FAssetPath FinalPath;
		FAssetPath PackagePath;
		uint64 CatalogRevision = 0;
		std::vector<FAssetPath> RedirectChain;
		std::string FinalAssetClassName;
		EAssetError Error = EAssetError::None;
		std::string ErrorMessage;
		// Counts package-file reads across the root load and its dependency closure.
		uint64 PackageFileReadCount = 0;
		std::vector<FAssetLoadMutation> Mutations;
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence;

		ASSETCORE_API auto HasNonUpgradeMutations() const -> bool;
	};

	// Adds a mutation to the active root load report. Calls outside package loading are ignored.
	ASSETCORE_API auto ReportAssetLoadMutation(
		DObject* Object,
		std::string HandlerId,
		std::string Summary,
		EAssetLoadMutationKind Kind = EAssetLoadMutationKind::NonUpgrade) -> void;

	// Identifies the exact authored package bytes represented by an audit result.
	struct FAssetPackageFingerprint
	{
		uintmax_t FileSize = 0;
		int64 LastWriteTimeTicks = 0;
		FXxHash128 ContentHash;
		uint32 ReaderVersion = 0;

		auto operator==(const FAssetPackageFingerprint&) const -> bool = default;
	};

	struct FAssetPackageSaveOptions {};

	// Captures package ownership before a higher-level load request begins.
	struct FAssetPackageLoadSnapshot
	{
		std::vector<FAssetPath> ResidentPackages;
	};

	// Newly created packages and packages backed by persistent catalog entries
	// share one residency store. Dirty state remains an independent DPackage bit.
	enum class EAssetPackagePublicationState : uint8
	{
		NewlyCreated,
		Published,
	};

	// Unload preserves unsaved work unless the caller explicitly accepts loss.
	enum class EAssetPackageUnloadPolicy : uint8
	{
		RejectUnsaved,
		DiscardUnsaved,
	};

	// Carries package metadata parsed without materializing package objects.
	struct FAssetPackageHeader
	{
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		uint64 ObjectCount = 0;

		// Number of source bytes consumed while parsing the header.
		uint64 BytesRead = 0;

		// Number of bytes physically read by the file-backed discovery operation.
		uint64 FileBytesRead = 0;
	};

	// Reads and validates only the package metadata needed by discovery. BytesRead is exposed
	// so diagnostics and tests can verify that object payloads were not consumed.
	ASSETCORE_API auto ReadAssetPackageHeader(std::string_view PhysicalPath, FAssetPackageHeader& OutHeader) -> FAssetResult;
	ASSETCORE_API auto ValidateAssetPackageBytes(std::span<const uint8> Bytes) -> FAssetResult;
	// Produces redirect-free package bytes for runtime publication without changing authored files.
	ASSETCORE_API auto CanonicalizeAssetPackageForCook(
		std::span<const uint8> Bytes,
		std::vector<uint8>& OutBytes) -> FAssetResult;

	struct FAssetPackageSerializationOptions
	{
		// Returning false omits the field record entirely from this serialization.
		std::function<bool(const DObject*, const FProperty*)> PropertyFilter;
	};

	enum class EAssetBundleSavePhase : uint8
	{
		CreateDirectories,
		StagePackage,
		PublishPackage,
		PublishRootPackage,
		PublishRegistry
	};

	struct FAssetBundleSaveOptions
	{
		// The root package is published after every dependency package.
		DPackage* RootPackage = nullptr;

		// Tests and higher-level transactions may stop immediately before a phase.
		std::function<bool(EAssetBundleSavePhase, size_t)> ShouldFail;
	};

	// Serializes an asset package without publishing it or changing dirty/registry state.
	ASSETCORE_API auto SerializeAssetPackageBytes(
		DPackage* Package,
		std::vector<uint8>& OutBytes,
		const FAssetPackageSerializationOptions& Options = {}) -> FAssetResult;

	// Serializes and stages every package before making any package or registry
	// entry visible. Any publication failure restores prior files and leaves
	// package dirty state and registry contents unchanged.
	ASSETCORE_API auto SavePackagesAtomically(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options = {}) -> FAssetResult;

	// Explicitly validates and publishes an existing mounted package before ordinary loading.
	ASSETCORE_API auto AdmitAssetPackageToCatalog(const FAssetPath& Path) -> FAssetResult;

	// Provides serialized main-asset fields without constructing objects or invoking PostLoad.
	enum class EAssetPackageObjectReferenceKind : uint8
	{
		Null,
		Internal,
		External
	};

	struct FAssetPackageObjectReference
	{
		EAssetPackageObjectReferenceKind Kind = EAssetPackageObjectReferenceKind::Null;
		uint64 ObjectId = 0;
		FAssetPath ExternalPath;
	};

	struct FAssetPackageField
	{
		std::string DeclaringClass;
		std::string Name;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;
		std::vector<uint8> Payload;
		uint32 SourceFormatVersion = 0;

		ASSETCORE_API auto TryReadString(std::string& OutValue) const -> bool;
		ASSETCORE_API auto TryReadStruct(DStruct* Struct, void* OutValue) const -> bool;
		ASSETCORE_API auto TryReadObjectReference(FAssetPackageObjectReference& OutValue) const -> bool;
		ASSETCORE_API auto TryReadObjectReferenceArray(
			std::vector<FAssetPackageObjectReference>& OutValues) const -> bool;

		template<typename T>
		auto TryReadScalar(T& OutValue) const -> bool
		{
			static_assert(std::is_trivially_copyable_v<T>);
			if (Payload.size() != sizeof(T)) return false;
			std::memcpy(&OutValue, Payload.data(), sizeof(T));
			return true;
		}
	};

	// Carries one serialized object and all of its fields without constructing it.
	struct FAssetPackageObjectInspection
	{
		uint64 Id = 0;
		uint64 OuterId = 0;
		std::string ClassName;
		std::string ObjectName;
		std::string ObjectPath;
		std::vector<FAssetPackageField> Fields;

		auto FindField(std::string_view Name) const -> const FAssetPackageField*
		{
			const auto It = std::ranges::find(Fields, Name, &FAssetPackageField::Name);
			return It == Fields.end() ? nullptr : &*It;
		}
	};

	// Carries every serialized object for lightweight tooling inspection.
	struct FAssetPackageInspection
	{
		FAssetPackageHeader Header;
		FAssetPackageFingerprint Fingerprint;
		std::vector<FAssetPackageObjectInspection> Objects;

		auto FindField(std::string_view Name) const -> const FAssetPackageField*
		{
			return Objects.empty() ? nullptr : Objects.front().FindField(Name);
		}

		auto FindObject(uint64 Id) const -> const FAssetPackageObjectInspection*
		{
			if (Id == 0 || Id > Objects.size()) return nullptr;
			const FAssetPackageObjectInspection& Object = Objects[static_cast<size_t>(Id - 1)];
			return Object.Id == Id ? &Object : nullptr;
		}
	};

	// Reads the complete serialized package into a field snapshot without resolving dependencies,
	// constructing objects, or invoking PostLoad.
	ASSETCORE_API auto InspectAssetPackage(std::string_view PhysicalPath, FAssetPackageInspection& OutInspection) -> FAssetResult;

	// Classifies one authored package edge in the unified reference graph.
}
