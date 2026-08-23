#pragma once

#include "Asset/CookedAsset.h"

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

	ASSETCORE_API auto EncodeCookedBulk(
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
		std::vector<std::byte>& OutBytes,
		std::string* OutError = nullptr
	) -> bool;
	ASSETCORE_API auto DecodeCookManifest(
		std::span<const std::byte> Bytes,
		FCookManifest& OutManifest,
		std::string* OutError = nullptr
	) -> bool;

	class FCookContext
	{
	public:
		using FPackageByteBuilder = std::function<bool(
			std::span<const FCookedPayloadDescriptor>,
			std::vector<std::byte>&,
			std::string*
		)>;

		ASSETCORE_API FCookContext(
			std::filesystem::path InCookRoot,
			ECookTargetPlatform InTargetPlatform,
			ECookTargetProfile InTargetProfile,
			bool bInRetainEditorOnlyData = false
		);
		ASSETCORE_API auto AddPackage(
			std::string VirtualPackagePath,
			std::vector<std::byte> PackageBytes,
			std::vector<FCookedBulkPayload> Payloads,
			std::string* OutError = nullptr
		) -> bool;
		ASSETCORE_API auto AddPackage(
			std::string VirtualPackagePath,
			std::vector<FCookedBulkPayload> Payloads,
			FPackageByteBuilder BuildPackageBytes,
			std::string* OutError = nullptr
		) -> bool;
		ASSETCORE_API auto Publish(std::string* OutError = nullptr) -> bool;
		auto GetTargetPlatform() const -> ECookTargetPlatform { return TargetPlatform; }
		auto GetTargetProfile() const -> ECookTargetProfile { return TargetProfile; }
		auto IsRetainingEditorOnlyData() const -> bool { return bRetainEditorOnlyData; }
		ASSETCORE_API auto MakePackageSerializationOptions() const
			-> FAssetPackageSerializationOptions;

	private:
		struct FPendingPackage
		{
			std::string VirtualPath;
			std::vector<std::byte> PackageBytes;
			std::vector<std::byte> BulkBytes;
		};

		std::filesystem::path CookRoot;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		bool bRetainEditorOnlyData = false;
		std::vector<FPendingPackage> Packages;
	};
} // namespace Durin::Asset
