#pragma once

#include "AssetCoreAPI.h"
#include "Serialization/Archive.h"

#include <functional>

namespace Durin::Asset
{
	inline constexpr uint64 AuthoredBulkExternalThreshold = 256ull * 1024;

	// Selects whether authored payload bytes accompany their descriptor in DAST or a local companion.
	enum class EAuthoredBulkStorageKind : uint8 { Inline, External };

	// Persists placement-independent authored payload identity plus its selected local placement.
	struct FAuthoredBulkDataDescriptor
	{
		FGuid PayloadId;
		FGuid FormatId;
		uint32 FormatVersion = 0;
		uint64 LogicalByteCount = 0;
		uint64 StoredByteCount = 0;
		FXxHash128 ContentHash;
		FXxHash128 ContainerHash;
		EAuthoredBulkStorageKind StorageKind = EAuthoredBulkStorageKind::Inline;

		auto operator==(const FAuthoredBulkDataDescriptor&) const -> bool = default;
	};

	// Carries a verified external authored payload from package serialization into publication.
	struct FAuthoredBulkPayload
	{
		FAuthoredBulkDataDescriptor Descriptor;
		FSharedByteBuffer Buffer;
	};

	// Owns one atomic authored payload with explicit synchronous residency and detached replacement.
	class FAuthoredBulkData
	{
	public:
		using FLoadFunction = std::function<bool(FSharedByteBuffer&, std::string&)>;

		FAuthoredBulkData() = default;
		ASSETCORE_API FAuthoredBulkData(FGuid PayloadId, FGuid FormatId, uint32 FormatVersion);

		auto GetDescriptor() const -> const FAuthoredBulkDataDescriptor& { return Descriptor; }
		auto GetResidency() const -> EArchiveBulkDataResidency { return Residency; }
		auto GetFailure() const -> std::string_view { return Failure; }
		auto IsResident() const -> bool { return Residency == EArchiveBulkDataResidency::Resident; }
		auto GetResidentBytes() const -> std::span<const std::byte>
		{
			return IsResident() ? Buffer.GetBytes() : std::span<const std::byte>();
		}

		ASSETCORE_API auto ReplaceBytes(std::span<const std::byte> Bytes) -> bool;
		ASSETCORE_API auto ReplaceBytes(
			FGuid PayloadId, FGuid FormatId, uint32 FormatVersion,
			std::span<const std::byte> Bytes) -> bool;
		ASSETCORE_API auto SetUnloaded(
			FAuthoredBulkDataDescriptor InDescriptor, FLoadFunction InLoader,
			std::string* OutError = nullptr) -> bool;
		ASSETCORE_API auto LoadSynchronous(std::string& OutError) -> bool;
		ASSETCORE_API auto Serialize(FArchive& Ar) -> void;
		ASSETCORE_API auto Identical(const FAuthoredBulkData& Other) const -> bool;

	private:
		FAuthoredBulkDataDescriptor Descriptor;
		EArchiveBulkDataResidency Residency = EArchiveBulkDataResidency::Resident;
		FSharedByteBuffer Buffer;
		FLoadFunction Loader;
		std::string Failure;
		bool bHashVerified = true;
	};
}
