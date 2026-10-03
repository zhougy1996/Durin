#pragma once

#include "CoreMinimal.h"

#include "DerivedDataCache/DerivedDataCacheTypes.h"

namespace Durin::DerivedData
{
	class FValueId
	{
	public:
		static constexpr uint32 MaximumIndex = 0x00ffffffu;
		constexpr FValueId() = default;
		// Convenience construction is deterministic and equivalent to FromName.
		DERIVEDDATACACHE_API FValueId(std::string_view Name);
		DERIVEDDATACACHE_API static auto FromName(std::string_view Name) -> FValueId;
		DERIVEDDATACACHE_API static auto FromHash(FXxHash128 Hash) -> FValueId;
		DERIVEDDATACACHE_API static auto FromBytes(std::array<uint8, 12> Bytes) -> FValueId;
		DERIVEDDATACACHE_API auto MakeIndexed(uint32 Index) const -> FValueId;
		auto IsNull() const -> bool { return std::ranges::all_of(Bytes, [](uint8 V) { return V == 0; }); }
		auto GetBytes() const -> const std::array<uint8, 12>& { return Bytes; }
		auto operator==(const FValueId&) const -> bool = default;
		auto operator<=>(const FValueId&) const = default;
	private:
		std::array<uint8, 12> Bytes{};
	};

	struct FValueIdHash
	{
		auto operator()(const FValueId& Id) const noexcept -> size_t
		{
			size_t Result = 0;
			for (uint8 Byte : Id.GetBytes()) Result = (Result * 16777619u) ^ Byte;
			return Result;
		}
	};

	class FValue
	{
	public:
		FValue() = default;
		DERIVEDDATACACHE_API explicit FValue(FSharedByteBuffer Data);
		auto GetRawHash() const -> FXxHash128 { return RawHash; }
		auto GetRawSize() const -> uint64 { return RawSize; }
		auto HasData() const -> bool { return !Data.IsEmpty() || RawSize == 0; }
		auto GetData() const -> FSharedByteBuffer { return Data; }
	private:
		friend class FCacheRecord;
		FXxHash128 RawHash;
		uint64 RawSize = 0;
		FSharedByteBuffer Data;
	};

	struct FValueWithId
	{
		FValueId Id;
		FValue Value;
	};

	struct FBuildValueKey
	{
		FCacheKey BuildKey;
		FValueId Id;
		auto operator==(const FBuildValueKey&) const -> bool = default;
	};
}
