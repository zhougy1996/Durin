#pragma once

#include "Serialization/BinaryEnvelope.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	constexpr FGuid FirstFormatId{0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff};
	constexpr FGuid SecondFormatId{0xfedcba98, 0x76544321, 0x81234567, 0x89abcdef};
	constexpr FBinaryEnvelopeLimits TestLimits{4096, 16384};

	template<typename T>
	auto ReferenceWrite(Durin::FMutableByteView Bytes, size_t Offset, T Value) -> void
	{
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Bytes[Offset + Index] = static_cast<std::byte>((Value >> (Index * 8)) & 0xff);
	}

	template<typename T>
	auto ReferenceRead(Durin::FByteView Bytes, size_t Offset) -> T
	{
		T Value = 0;
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Value |= static_cast<T>(std::to_integer<uint8>(Bytes[Offset + Index])) << (Index * 8);
		return Value;
	}

	auto ReferenceEncode(FGuid FormatId, uint32 FormatVersion, uint32 RequiredFeatures,
		Durin::FByteView FormatHeader, uint64 FileBytes) -> Durin::FByteBuffer
	{
		Durin::FByteBuffer Bytes(64 + FormatHeader.size());
		Bytes[0] = std::byte{0x44};
		Bytes[1] = std::byte{0x55};
		Bytes[2] = std::byte{0x52};
		Bytes[3] = std::byte{0x46};
		ReferenceWrite<uint16>(Bytes, 4, 1);
		ReferenceWrite<uint16>(Bytes, 6, 64);
		ReferenceWrite<uint32>(Bytes, 8, FormatId.A);
		ReferenceWrite<uint32>(Bytes, 12, FormatId.B);
		ReferenceWrite<uint32>(Bytes, 16, FormatId.C);
		ReferenceWrite<uint32>(Bytes, 20, FormatId.D);
		ReferenceWrite<uint32>(Bytes, 24, FormatVersion);
		ReferenceWrite<uint32>(Bytes, 28, RequiredFeatures);
		ReferenceWrite<uint64>(Bytes, 32, Bytes.size());
		ReferenceWrite<uint64>(Bytes, 40, FileBytes);
		std::ranges::copy(FormatHeader, Bytes.begin() + 64);
		const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
		ReferenceWrite<uint64>(Bytes, 48, Hash.HashLow);
		ReferenceWrite<uint64>(Bytes, 56, Hash.HashHigh);
		return Bytes;
	}

	auto ReferenceParse(Durin::FByteView Bytes, uint64 PhysicalFileBytes,
		FBinaryEnvelopePreamble& OutPreamble) -> bool
	{
		if (Bytes.size() < 64 || Bytes[0] != std::byte{0x44} || Bytes[1] != std::byte{0x55}
			|| Bytes[2] != std::byte{0x52} || Bytes[3] != std::byte{0x46}
			|| ReferenceRead<uint16>(Bytes, 4) != 1 || ReferenceRead<uint16>(Bytes, 6) != 64)
			return false;
		FBinaryEnvelopePreamble Parsed{
			.FormatId = {
				ReferenceRead<uint32>(Bytes, 8), ReferenceRead<uint32>(Bytes, 12),
				ReferenceRead<uint32>(Bytes, 16), ReferenceRead<uint32>(Bytes, 20)},
			.FormatVersion = ReferenceRead<uint32>(Bytes, 24),
			.RequiredFeatures = ReferenceRead<uint32>(Bytes, 28),
			.HeaderBytes = ReferenceRead<uint64>(Bytes, 32),
			.FileBytes = ReferenceRead<uint64>(Bytes, 40),
			.HeaderHash = {
				ReferenceRead<uint64>(Bytes, 48), ReferenceRead<uint64>(Bytes, 56)}};
		if (!Parsed.FormatId.IsValid() || Parsed.FormatVersion == 0
			|| Parsed.HeaderBytes != Bytes.size() || Parsed.HeaderBytes < 64
			|| Parsed.HeaderBytes > Parsed.FileBytes || Parsed.FileBytes != PhysicalFileBytes)
			return false;
		Durin::FByteBuffer Zeroed(Bytes.begin(), Bytes.end());
		std::ranges::fill(std::span(Zeroed).subspan(48, 16), std::byte{});
		if (FXxHash128::HashBuffer(Zeroed) != Parsed.HeaderHash) return false;
		OutPreamble = Parsed;
		return true;
	}

	auto Hex(Durin::FByteView Bytes) -> std::string
	{
		std::string Result;
		for (std::byte Byte : Bytes) Result += std::format("{:02x}", std::to_integer<uint8>(Byte));
		return Result;
	}

	auto ReferenceRehash(Durin::FByteBuffer& Bytes) -> void
	{
		std::ranges::fill(std::span(Bytes).subspan(48, 16), std::byte{});
		const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
		ReferenceWrite<uint64>(Bytes, 48, Hash.HashLow);
		ReferenceWrite<uint64>(Bytes, 56, Hash.HashHigh);
	}

	auto MakeDescriptor(FGuid FormatId = FirstFormatId,
		std::string Name = "Durin.BinaryFormat.Test") -> FBinaryFormatDescriptor
	{
		return {
			.FormatId = FormatId,
			.DebugName = std::move(Name),
			.MinimumFormatVersion = 2,
			.MaximumFormatVersion = 4,
			.SupportedRequiredFeatures = 0x00000005,
			.Limits = TestLimits};
	}

	auto MakeRegistry(std::span<const FBinaryFormatDescriptor> Descriptors)
		-> FBinaryFormatRegistry
	{
		FBinaryFormatRegistry Registry;
		EXPECT_TRUE(FBinaryFormatRegistry::Create(Descriptors, Registry));
		return Registry;
	}
}
