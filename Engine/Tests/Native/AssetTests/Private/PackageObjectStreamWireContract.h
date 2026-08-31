#pragma once

#include "Misc/CoreTypes.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Durin::Testing::PackageObjectStream
{
	constexpr uint32 Magic = 0x54534144;
	constexpr uint32 Version = 5;
	constexpr uint64 MaximumPackageBytes = 256ull * 1024ull * 1024ull;
	constexpr uint64 MaximumStringBytes = 1024ull * 1024ull;
	constexpr uint32 MaximumSummaryBytes = 65535;
	constexpr uint64 MaximumDependencies = 4096;
	constexpr uint64 MaximumObjects = 1048575;
	constexpr uint8 SectionCount = 5;

	enum class ESectionKind : uint8
	{
		Name = 0x01,
		Type = 0x02,
		Schema = 0x03,
		Object = 0x04,
		Value = 0x05,
	};

	class FWireWriter
	{
	public:
		auto WriteU8(uint8 Value) -> void;
		auto WriteU16(uint16 Value) -> void;
		auto WriteU32(uint32 Value) -> void;
		auto WriteU64(uint64 Value) -> void;
		auto WriteF32(float Value) -> void;
		auto WriteF64(double Value) -> void;
		auto WriteVarUInt(uint64 Value) -> void;
		auto WriteVarInt(int64 Value) -> void;
		auto WriteString(std::string_view Value, std::string& OutError) -> bool;
		auto WriteBytes(std::span<const std::byte> Value) -> void;

		auto Bytes() const -> const Durin::FByteArray& { return Data; }
		auto TakeBytes() -> Durin::FByteArray { return std::move(Data); }

	private:
		Durin::FByteArray Data;
	};

	class FWireReader
	{
	public:
		explicit FWireReader(std::span<const std::byte> InBytes) : Bytes(InBytes) {}

		auto ReadU8(uint8& OutValue, std::string& OutError) -> bool;
		auto ReadU16(uint16& OutValue, std::string& OutError) -> bool;
		auto ReadU32(uint32& OutValue, std::string& OutError) -> bool;
		auto ReadU64(uint64& OutValue, std::string& OutError) -> bool;
		auto ReadF32(float& OutValue, std::string& OutError) -> bool;
		auto ReadF64(double& OutValue, std::string& OutError) -> bool;
		auto ReadVarUInt(uint64& OutValue, std::string& OutError) -> bool;
		auto ReadVarInt(int64& OutValue, std::string& OutError) -> bool;
		auto ReadString(std::string& OutValue, std::string& OutError) -> bool;
		auto ReadBytes(uint64 Count, std::span<const std::byte>& OutValue, std::string& OutError) -> bool;
		auto RequireEnd(std::string& OutError) const -> bool;

		auto Remaining() const -> uint64 { return Bytes.size() - Offset; }
		auto Position() const -> uint64 { return Offset; }

	private:
		std::span<const std::byte> Bytes;
		uint64 Offset = 0;
	};

	struct FPublicSummary
	{
		std::string AssetClass;
		uint8 EntryKind = 0;
		std::string RedirectDestination;
		std::vector<std::string> Dependencies;
		uint64 ObjectCount = 0;

		auto operator==(const FPublicSummary&) const -> bool = default;
	};

	struct FSectionEntry
	{
		ESectionKind Kind = ESectionKind::Name;
		uint32 Offset = 0;
		uint32 Length = 0;

		auto operator==(const FSectionEntry&) const -> bool = default;
	};

	struct FValidatedHeader
	{
		FPublicSummary Summary;
		std::array<FSectionEntry, SectionCount> Sections;

		auto operator==(const FValidatedHeader&) const -> bool = default;
	};

	auto IsValidUtf8(std::string_view Value) -> bool;
	auto EncodePublicSummary(
		const FPublicSummary& Summary,
		Durin::FByteArray& OutBytes,
		std::string& OutError) -> bool;
	auto EncodeEnvelope(
		const FPublicSummary& Summary,
		const std::array<Durin::FByteArray, SectionCount>& Sections,
		Durin::FByteArray& OutBytes,
		std::string& OutError) -> bool;
	auto DecodeHeader(
		std::span<const std::byte> Bytes,
		FValidatedHeader& OutHeader,
		std::string& OutError) -> bool;
}
