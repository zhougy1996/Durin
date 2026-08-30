#pragma once

#include "EngineAPI.h"
#include "Asset/CookedAsset.h"
#include "Asset/PackageBulkData.h"

namespace Durin::Asset
{
	struct FAssetPackageSerializationOptions;
	struct FCookedBulkPayload
	{
		FGuid PayloadId;
		uint32 Flags = 1;
		uint32 PayloadSchemaVersion = 0;
		ECookedPayloadCompression Compression = ECookedPayloadCompression::None;
		uint32 Alignment = 16;
		std::vector<std::byte> Bytes;
	};

	ENGINE_API auto EncodeCookedBulk(
		std::span<const FCookedBulkPayload> Payloads,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		std::vector<std::byte>& OutBytes,
		std::vector<FCookedPayloadDescriptor>* OutDescriptors = nullptr,
		std::string* OutError = nullptr
	) -> bool;

	enum class ECookManifestEntryKind : uint8
	{
		CookedPackage = 1,
		CookedBulk = 2,
		PackageBulk = 3,
	};

	inline constexpr uint8 CookManifestEntryPresent = 1u << 0;
	inline constexpr uint8 CookManifestEntryCookedFieldProjection = 1u << 1;
	inline constexpr uint8 CookManifestEntryKnownFlags =
		CookManifestEntryPresent | CookManifestEntryCookedFieldProjection;

	struct FCookManifestEntry
	{
		ECookManifestEntryKind Kind = ECookManifestEntryKind::CookedPackage;
		uint8 Flags = CookManifestEntryPresent;
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

	ENGINE_API auto EncodeCookManifest(
		const FCookManifest& Manifest,
		std::vector<std::byte>& OutBytes,
		std::string* OutError = nullptr
	) -> bool;
	ENGINE_API auto DecodeCookManifest(
		std::span<const std::byte> Bytes,
		FCookManifest& OutManifest,
		std::string* OutError = nullptr
	) -> bool;

	class FCookContext
	{
	public:
		ENGINE_API FCookContext(
			std::filesystem::path InCookRoot,
			ECookTargetPlatform InTargetPlatform,
			ECookTargetProfile InTargetProfile,
			bool bInRetainEditorOnlyData = false
		);
		ENGINE_API auto AddPackage(
			std::string VirtualPackagePath,
			std::vector<std::byte> PackageBytes,
			std::string* OutError = nullptr
		) -> bool;
		ENGINE_API auto AddPackage(
			std::string VirtualPackagePath,
			DPackage* Package,
			std::string* OutError = nullptr
		) -> bool;
		// Publishes an opaque headerless raw segment owned by a higher-level
		// manifest rather than by reflected package fields.
		ENGINE_API auto AddRawPackage(
			std::string VirtualPackagePath,
			std::vector<std::byte> PackageBytes,
			std::vector<std::byte> RawSegmentBytes,
			std::string* OutError = nullptr
		) -> bool;
		ENGINE_API auto Publish(std::string* OutError = nullptr) -> bool;
		auto GetTargetPlatform() const -> ECookTargetPlatform { return TargetPlatform; }
		auto GetTargetProfile() const -> ECookTargetProfile { return TargetProfile; }
		auto IsRetainingEditorOnlyData() const -> bool { return bRetainEditorOnlyData; }
		ENGINE_API auto MakePackageSerializationOptions() const
			-> FAssetPackageSerializationOptions;

	private:
		struct FPendingPackage
		{
			std::string VirtualPath;
			std::vector<std::byte> PackageBytes;
			std::vector<std::byte> BulkBytes;
			FPackageBulkSegmentSummary BulkSummary;
			std::vector<FPackageBulkDataEntry> BulkEntries;
			bool bRawBulkSegment = false;
			bool bOpaqueRawSegment = false;
		};

		std::filesystem::path CookRoot;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		bool bRetainEditorOnlyData = false;
		std::vector<FPendingPackage> Packages;
	};
} // namespace Durin::Asset
