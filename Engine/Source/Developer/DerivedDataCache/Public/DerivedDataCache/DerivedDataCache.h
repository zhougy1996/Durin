#pragma once

#include "DerivedDataCacheAPI.h"
#include "Hash/XxHash.h"
#include "Serialization/SharedByteBuffer.h"
#include <expected>
#include <optional>

namespace Durin::DerivedData
{
	// Identifies a validated logical cache namespace independent of its backend.
	class FCacheBucket
	{
	public:
		static constexpr size_t MaximumNameLength = 63;
		DERIVEDDATACACHE_API static auto FromString(
			std::string_view Value, std::string* OutError = nullptr) -> FCacheBucket;
		auto IsValid() const -> bool { return Name != nullptr; }
		DERIVEDDATACACHE_API auto ToString() const -> std::string_view;
		auto operator==(const FCacheBucket&) const -> bool = default;

	private:
		const char* Name = nullptr;
	};

	// Identifies one cache record by its logical namespace and binary identity.
	class FCacheKey
	{
	public:
		DERIVEDDATACACHE_API static auto FromHash(
			FCacheBucket Bucket, FXxHash128 Hash) -> FCacheKey;
		DERIVEDDATACACHE_API static auto FromString(
			FCacheBucket Bucket, std::string_view Hash,
			std::string* OutError = nullptr) -> FCacheKey;
		auto IsValid() const -> bool { return Bucket.IsValid() && !Hash.IsZero(); }
		auto GetBucket() const -> const FCacheBucket& { return Bucket; }
		auto GetHash() const -> const FXxHash128& { return Hash; }
		DERIVEDDATACACHE_API auto ToString() const -> std::string;
		auto operator==(const FCacheKey&) const -> bool = default;

	private:
		FCacheBucket Bucket;
		FXxHash128 Hash;
	};

	// Classifies cache failures. A missing entry is represented by an empty
	// optional in FCacheGetResult and is never a failure.
	enum class ECacheError : uint8
	{
		InvalidRequest,
		ValueTooLarge,
		Corrupt,
		StorageFailure
	};

	struct FCacheError
	{
		ECacheError Code;
		std::string Diagnostic;
	};

	// Supplies the logical identity and caller-enforced read bound for one lookup.
	struct FCacheGetRequest
	{
		FCacheKey Key;
		uint64 MaximumValueBytes = 0;
	};

	// A successful lookup contains either immutable hit bytes or an empty miss.
	using FCacheGetResult = std::expected<std::optional<FSharedByteBuffer>, FCacheError>;

	// Borrows entry bytes only for the duration of a synchronous put call.
	struct FCachePutRequest
	{
		FCacheKey Key;
		FByteView Value;
		uint64 MaximumValueBytes = 0;
	};

	// Reports publication outcome without exposing backend details.
	using FCachePutResult = std::expected<void, FCacheError>;

	// Provides synchronous backend-neutral access to process derived data.
	class FDerivedDataCache
	{
	public:
		DERIVEDDATACACHE_API auto Get(const FCacheGetRequest& Request) const -> FCacheGetResult;
		DERIVEDDATACACHE_API auto Put(const FCachePutRequest& Request) const -> FCachePutResult;
	};

	// Returns the process-owned cache facade. Calls remain synchronous and do not
	// retain backend paths or mutable caller buffers.
	DERIVEDDATACACHE_API auto GetCache() -> FDerivedDataCache&;
}
