#pragma once

#include "DerivedDataCacheAPI.h"
#include "Serialization/SharedByteBuffer.h"

namespace Durin::DerivedData
{
	// Identifies a validated logical cache namespace independent of its backend.
	class FCacheBucket
	{
	public:
		DERIVEDDATACACHE_API static auto FromString(
			std::string_view Value, std::string* OutError = nullptr) -> FCacheBucket;
		auto IsValid() const -> bool { return !Value.empty(); }
		auto ToString() const -> std::string_view { return Value; }
		auto operator==(const FCacheBucket&) const -> bool = default;

	private:
		std::string Value;
	};

	// Identifies one canonical lowercase 128-bit cache entry.
	class FCacheKey
	{
	public:
		DERIVEDDATACACHE_API static auto FromString(
			std::string_view Value, std::string* OutError = nullptr) -> FCacheKey;
		auto IsValid() const -> bool { return Value.size() == 32; }
		auto ToString() const -> std::string_view { return Value; }
		auto operator==(const FCacheKey&) const -> bool = default;

	private:
		std::string Value;
	};

	// Classifies a synchronous cache lookup without interpreting entry bytes.
	enum class ECacheGetStatus : uint8
	{
		Hit,
		Miss,
		InvalidRequest,
		ValueTooLarge,
		StorageFailure
	};

	// Supplies the logical identity and caller-enforced read bound for one lookup.
	struct FCacheGetRequest
	{
		FCacheBucket Bucket;
		FCacheKey Key;
		uint64 MaximumValueBytes = 0;
	};

	// Returns immutable bytes only for a successful cache hit.
	struct FCacheGetResult
	{
		ECacheGetStatus Status = ECacheGetStatus::Miss;
		FSharedByteBuffer Value;
		std::string Diagnostic;
		explicit operator bool() const { return Status == ECacheGetStatus::Hit; }
	};

	// Classifies synchronous immutable entry publication.
	enum class ECachePutStatus : uint8
	{
		Stored,
		InvalidRequest,
		ValueTooLarge,
		StorageFailure
	};

	// Borrows entry bytes only for the duration of a synchronous put call.
	struct FCachePutRequest
	{
		FCacheBucket Bucket;
		FCacheKey Key;
		std::span<const std::byte> Value;
		uint64 MaximumValueBytes = 0;
	};

	// Reports publication outcome without exposing backend details.
	struct FCachePutResult
	{
		ECachePutStatus Status = ECachePutStatus::StorageFailure;
		std::string Diagnostic;
		explicit operator bool() const { return Status == ECachePutStatus::Stored; }
	};

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
