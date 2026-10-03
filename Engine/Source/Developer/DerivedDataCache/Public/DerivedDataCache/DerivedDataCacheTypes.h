#pragma once

#include "CoreMinimal.h"

#include "DerivedDataCacheAPI.h"

namespace Durin::DerivedData
{
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
}
