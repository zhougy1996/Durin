#include "DerivedDataValue.h"

#include <cstring>

namespace Durin::DerivedData
{
	FValueId::FValueId(std::string_view Name) : FValueId(FromName(Name)) {}
	auto FValueId::FromName(std::string_view Name) -> FValueId
	{
		return FromHash(FXxHash128::HashBuffer(std::as_bytes(std::span(Name))));
	}
	auto FValueId::FromHash(FXxHash128 Hash) -> FValueId
	{
		FValueId Result;
		for (uint32 Index = 0; Index < 8; ++Index) Result.Bytes[Index] = uint8(Hash.HashLow >> (Index * 8));
		for (uint32 Index = 0; Index < 4; ++Index) Result.Bytes[Index + 8] = uint8(Hash.HashHigh >> (Index * 8));
		if (Result.IsNull()) Result.Bytes[0] = 1;
		return Result;
	}
	auto FValueId::FromBytes(std::array<uint8, 12> Bytes) -> FValueId
	{
		FValueId Result; Result.Bytes = Bytes; return Result;
	}
	auto FValueId::MakeIndexed(uint32 Index) const -> FValueId
	{
		if (IsNull() || Index > MaximumIndex) return {};
		auto Result = *this;
		Result.Bytes[9] ^= uint8(Index);
		Result.Bytes[10] ^= uint8(Index >> 8);
		Result.Bytes[11] ^= uint8(Index >> 16);
		if (Result.IsNull()) Result.Bytes[0] ^= 1;
		return Result;
	}

	FValue::FValue(FSharedByteBuffer InData)
		: RawHash(FXxHash128::HashBuffer(InData.GetBytes())), RawSize(InData.GetSize()), Data(std::move(InData)) {}
}
