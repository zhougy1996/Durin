#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"
#include "Serialization/SharedByteBuffer.h"

namespace Durin::Asset
{
	// Identifies opaque bytes independently of domain meaning and physical placement.
	struct FBulkDataDescriptor
	{
		FGuid PayloadId;
		uint64 LogicalByteCount = 0;
		FXxHash128 ContentHash;

		auto operator==(const FBulkDataDescriptor&) const -> bool = default;
	};

	ENGINE_API auto ValidateBulkDataDescriptor(
		const FBulkDataDescriptor& Descriptor,
		std::string* OutError = nullptr) -> bool;
	ENGINE_API auto VerifyBulkDataBuffer(
		const FBulkDataDescriptor& Descriptor,
		const FSharedByteBuffer& Buffer,
		std::string* OutError = nullptr) -> bool;

	// Owns placement-independent identity and verified immutable resident bytes.
	class FBulkData
	{
	public:
		FBulkData() = default;

		ENGINE_API static auto TryCreate(
			FBulkDataDescriptor Descriptor,
			FSharedByteBuffer Buffer,
			FBulkData& OutValue,
			std::string* OutError = nullptr) -> bool;

		auto HasPayload() const -> bool { return Descriptor.PayloadId.IsValid(); }
		auto GetDescriptor() const -> const FBulkDataDescriptor& { return Descriptor; }
		auto GetBytes() const -> std::span<const std::byte> { return Buffer.GetBytes(); }
		auto GetBuffer() const -> const FSharedByteBuffer& { return Buffer; }

	private:
		FBulkDataDescriptor Descriptor;
		FSharedByteBuffer Buffer;
	};
}
