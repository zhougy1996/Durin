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

	// Version 1 addresses trusted local blobs using XXH3-128 over raw bytes.
	// Future cryptographic identities require a new version and storage namespace.
	struct FCacheBlobIdentity
	{
		static constexpr uint32 Version = 1;
		static constexpr std::string_view Algorithm = "xxh3-128";
		static constexpr std::string_view Namespace = "values/v1";
		FXxHash128 Hash;
		auto operator==(const FCacheBlobIdentity&) const -> bool = default;
	};

	class FValue
	{
	public:
		FValue() = default;
		DERIVEDDATACACHE_API explicit FValue(FSharedByteBuffer Data);
		static auto FromReference(FXxHash128 Hash, uint64 Size) -> FValue
		{
			FValue Result; Result.RawHash = Hash; Result.RawSize = Size; return Result;
		}
		// A reference retains identity and size but has no resident payload.
		auto RemoveData() const -> FValue
		{
			auto Result = *this;
			Result.Data = {};
			Result.DataPresent = false;
			return Result;
		}
		auto GetRawHash() const -> FXxHash128 { return RawHash; }
		auto GetBlobIdentity() const -> FCacheBlobIdentity { return {RawHash}; }
		auto GetRawSize() const -> uint64 { return RawSize; }
		auto HasData() const -> bool { return DataPresent; }
		auto GetData() const -> FSharedByteBuffer { return Data; }
	private:
		friend class FCacheRecord;
		FXxHash128 RawHash;
		uint64 RawSize = 0;
		FSharedByteBuffer Data;
		bool DataPresent = false;
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
