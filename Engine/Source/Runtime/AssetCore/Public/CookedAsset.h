#pragma once

#include "AssetCoreAPI.h"
#include "DObject/CoreDObject.h"

#include "CookedAsset.gen.h"

namespace Durin::Asset
{
	enum class ECookTargetPlatform : uint32
	{
		Invalid = 0,
		Win64 = 1
	};

	enum class ECookTargetProfile : uint32
	{
		Invalid = 0,
		Game = 1,
		EditorValidation = 2
	};

	enum class ECookedPayloadCompression : uint32
	{
		None = 0,
		Zstandard = 1
	};

	enum class ECookedPayloadLocationKind : uint32
	{
		Invalid = 0,
		PackageCompanion = 1
	};

	enum class EPackageLoadMode : uint32
	{
		AuthoredEditor = 0,
		CookedRuntime = 1
	};

	// Selects one logical payload without persisting a workstation or companion path.
	DSTRUCT()
	struct FCookedPayloadDescriptor
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid PayloadId;

		DPROPERTY()
		uint32 LocationKind = 0;

		DPROPERTY()
		uint64 Offset = 0;

		DPROPERTY()
		uint64 StoredSize = 0;

		DPROPERTY()
		uint64 UncompressedSize = 0;

		DPROPERTY()
		uint32 Alignment = 0;

		DPROPERTY()
		uint64 PayloadHashLow = 0;

		DPROPERTY()
		uint64 PayloadHashHigh = 0;

		DPROPERTY()
		uint32 PayloadSchemaVersion = 0;

		DPROPERTY()
		uint32 TargetPlatform = 0;

		DPROPERTY()
		uint32 TargetProfile = 0;

		DPROPERTY()
		uint32 CompressionMethod = 0;

		auto operator==(const FCookedPayloadDescriptor&) const -> bool = default;
	};

	// Carries immutable process-wide storage policy into every package lookup.
	struct FPackageLoadContext
	{
		EPackageLoadMode Mode = EPackageLoadMode::AuthoredEditor;
		std::filesystem::path CookRoot;

		ASSETCORE_API auto IsValid(std::string* OutError = nullptr) const -> bool;
		auto AllowsSourceFallback() const -> bool { return Mode == EPackageLoadMode::AuthoredEditor; }
		auto AllowsDerivedDataFallback() const -> bool { return Mode == EPackageLoadMode::AuthoredEditor; }
	};

	// Supplies one uncompressed logical payload to deterministic DBLK construction.
	struct FCookedBulkPayload
	{
		FGuid PayloadId;
		uint32 Flags = 1;
		uint32 PayloadSchemaVersion = 0;
		ECookedPayloadCompression Compression = ECookedPayloadCompression::None;
		uint32 Alignment = 16;
		std::vector<uint8> Bytes;
	};

	// Owns validated DBLK entries and their uncompressed payload bytes.
	struct FCookedBulkContainer
	{
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::vector<FCookedPayloadDescriptor> Entries;
		std::vector<std::vector<uint8>> Payloads;
	};

	ASSETCORE_API auto EncodeCookedBulk(
		std::span<const FCookedBulkPayload> Payloads,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::vector<FCookedPayloadDescriptor>* OutDescriptors = nullptr,
		std::string* OutError = nullptr
	) -> bool;

	ASSETCORE_API auto DecodeCookedBulk(
		std::span<const uint8> Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FCookedBulkContainer& OutContainer,
		std::string* OutError = nullptr
	) -> bool;

	ASSETCORE_API auto LoadCookedBulkFile(
		const std::filesystem::path& Path,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FCookedBulkContainer& OutContainer,
		std::string* OutError = nullptr
	) -> bool;

	ASSETCORE_API auto ResolveCookedPayload(
		const FCookedBulkContainer& Container,
		const FCookedPayloadDescriptor& Descriptor,
		std::span<const uint8>& OutPayload,
		std::string* OutError = nullptr
	) -> bool;

	ASSETCORE_API auto ResolveCookedPackagePath(
		const std::filesystem::path& CookRoot,
		std::string_view VirtualPackagePath,
		std::filesystem::path& OutPackagePath,
		std::string* OutError = nullptr
	) -> bool;

	ASSETCORE_API auto ResolveCookedCompanionPath(
		const std::filesystem::path& CookRoot,
		const std::filesystem::path& PackagePath,
		std::filesystem::path& OutCompanionPath,
		std::string* OutError = nullptr
	) -> bool;

	enum class ECookManifestEntryKind : uint8
	{
		CookedPackage = 1,
		CookedBulk = 2
	};

	struct FCookManifestEntry
	{
		ECookManifestEntryKind Kind = ECookManifestEntryKind::CookedPackage;
		uint8 Flags = 1;
		std::string RelativePath;
		uint64 FileSize = 0;
		uint64 HashLow = 0;
		uint64 HashHigh = 0;

		auto operator==(const FCookManifestEntry&) const -> bool = default;
	};

	struct FCookManifest
	{
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::vector<FCookManifestEntry> Entries;
	};

	ASSETCORE_API auto EncodeCookManifest(
		const FCookManifest& Manifest,
		std::vector<uint8>& OutBytes,
		std::string* OutError = nullptr
	) -> bool;

	ASSETCORE_API auto DecodeCookManifest(
		std::span<const uint8> Bytes,
		FCookManifest& OutManifest,
		std::string* OutError = nullptr
	) -> bool;

	// Publishes sorted cooked packages, companions, and the manifest as one manifest-bounded transaction.
	class FCookContext
	{
	public:
		using FPackageByteBuilder = std::function<bool(
			std::span<const FCookedPayloadDescriptor>,
			std::vector<uint8>&,
			std::string*)>;

		ASSETCORE_API FCookContext(
			std::filesystem::path InCookRoot,
			ECookTargetPlatform InTargetPlatform,
			ECookTargetProfile InTargetProfile);

		ASSETCORE_API auto AddPackage(
			std::string VirtualPackagePath,
			std::vector<uint8> PackageBytes,
			std::vector<FCookedBulkPayload> Payloads,
			std::string* OutError = nullptr
		) -> bool;

		// Encodes payloads first so asset-specific code can serialize the exact logical descriptors.
		ASSETCORE_API auto AddPackage(
			std::string VirtualPackagePath,
			std::vector<FCookedBulkPayload> Payloads,
			FPackageByteBuilder BuildPackageBytes,
			std::string* OutError = nullptr
		) -> bool;

		ASSETCORE_API auto Publish(std::string* OutError = nullptr) -> bool;
		auto GetTargetPlatform() const -> ECookTargetPlatform { return TargetPlatform; }
		auto GetTargetProfile() const -> ECookTargetProfile { return TargetProfile; }

	private:
		struct FPendingPackage
		{
			std::string VirtualPath;
			std::vector<uint8> PackageBytes;
			std::vector<uint8> BulkBytes;
		};

		std::filesystem::path CookRoot;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::vector<FPendingPackage> Packages;
	};
}
