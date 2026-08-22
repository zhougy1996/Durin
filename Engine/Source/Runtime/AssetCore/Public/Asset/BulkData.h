#pragma once

#include "AssetCoreAPI.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"
#include "Serialization/SharedByteBuffer.h"

#include <memory>

namespace Durin::Asset
{
	// Identifies the authority and failure policy behind a physical bulk payload.
	enum class EBulkDataStorageDomain : uint8
	{
		None,
		Authored,
		Derived,
		Cooked
	};

	// Describes one payload independently of its physical placement or container.
	struct FBulkDataDescriptor
	{
		FGuid PayloadId;
		FGuid FormatId;
		uint32 FormatVersion = 0;
		uint64 LogicalByteCount = 0;
		uint64 StoredByteCount = 0;
		FXxHash128 ContentHash;

		auto operator==(const FBulkDataDescriptor&) const -> bool = default;
	};

	enum class EBulkDataResidency : uint8
	{
		Unloaded,
		Resident,
		Failed
	};

	ASSETCORE_API auto ValidateBulkDataDescriptor(
		const FBulkDataDescriptor& Descriptor,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto VerifyBulkDataBuffer(
		const FBulkDataDescriptor& Descriptor,
		const FSharedByteBuffer& Buffer,
		std::string* OutError = nullptr) -> bool;

	// Resolves bytes for one lifecycle domain without exposing paths, offsets, or mutation.
	class IBulkDataProvider
	{
	public:
		virtual ~IBulkDataProvider() = default;

		virtual auto GetStorageDomain() const -> EBulkDataStorageDomain = 0;
		virtual auto LoadSynchronous(
			const FBulkDataDescriptor& Descriptor,
			FSharedByteBuffer& OutBuffer,
			std::string& OutError) const -> bool = 0;
	};

	// Owns placement-independent identity, immutable bytes, and synchronous residency state.
	class FBulkData
	{
	public:
		FBulkData() = default;

		ASSETCORE_API static auto TryCreateResident(
			FBulkDataDescriptor Descriptor,
			FSharedByteBuffer Buffer,
			EBulkDataStorageDomain StorageDomain,
			FBulkData& OutValue,
			std::string* OutError = nullptr) -> bool;
		ASSETCORE_API static auto TryCreateUnloaded(
			FBulkDataDescriptor Descriptor,
			std::shared_ptr<const IBulkDataProvider> Provider,
			FBulkData& OutValue,
			std::string* OutError = nullptr) -> bool;

		auto HasPayload() const -> bool { return Descriptor.PayloadId.IsValid(); }
		auto GetDescriptor() const -> const FBulkDataDescriptor& { return Descriptor; }
		auto GetStorageDomain() const -> EBulkDataStorageDomain { return StorageDomain; }
		auto GetResidency() const -> EBulkDataResidency { return Residency; }
		auto GetFailure() const -> std::string_view { return Failure; }
		auto IsResident() const -> bool { return Residency == EBulkDataResidency::Resident; }
		auto GetResidentBytes() const -> std::span<const std::byte>
		{
			return IsResident() ? Buffer.GetBytes() : std::span<const std::byte>();
		}
		auto GetResidentBuffer() const -> const FSharedByteBuffer& { return Buffer; }

		ASSETCORE_API auto LoadSynchronous(std::string& OutError) -> bool;

	private:
		FBulkDataDescriptor Descriptor;
		EBulkDataStorageDomain StorageDomain = EBulkDataStorageDomain::None;
		EBulkDataResidency Residency = EBulkDataResidency::Resident;
		FSharedByteBuffer Buffer;
		std::shared_ptr<const IBulkDataProvider> Provider;
		std::string Failure;
	};
}
