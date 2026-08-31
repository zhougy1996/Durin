#pragma once

#include "CoreAPI.h"
#include "Serialization/Archive.h"

namespace Durin
{
	inline constexpr uint32 BinaryFormatMarker = 0x01020304;
	inline constexpr uint64 MaximumBinaryStringBytes = 1024ull * 1024ull;

	// Bounds one complete binary value and each variable-width field within it.
	struct FBinaryCursorLimits
	{
		uint64 MaximumTotalBytes = std::numeric_limits<uint64>::max();
		uint64 MaximumFieldBytes = std::numeric_limits<uint64>::max();
	};

	// Selects the byte order of an explicitly encoded fixed-width integer.
	enum class EBinaryByteOrder : uint8 { LittleEndian, BigEndian };

	template<typename T>
	concept CBinaryInteger = std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>;

	// Identifies a versioned binary family, schema, payload format, and byte order.
	struct FBinaryFormatHeader
	{
		uint32 Magic = 0;
		uint32 SchemaVersion = 0;
		uint32 FormatVersion = 0;
		uint32 Marker = BinaryFormatMarker;
	};

	// Builds bounded binary values through one Archive bound to its internal storage.
	// A limit failure is sticky for the current sequence and never partially appends one operation.
	class FBinaryWriter
	{
	public:
		explicit FBinaryWriter(FBinaryCursorLimits InLimits = {})
			: Limits(InLimits), Archive(Bytes, EArchivePurpose::DerivedDataPayload) {}
		FBinaryWriter(const FBinaryWriter&) = delete;
		auto operator=(const FBinaryWriter&) -> FBinaryWriter& = delete;
		FBinaryWriter(FBinaryWriter&&) = delete;
		auto operator=(FBinaryWriter&&) -> FBinaryWriter& = delete;

		// Writes one fixed-width integer in the selected byte order.
		template<CBinaryInteger T>
		auto WriteInteger(T Value, EBinaryByteOrder ByteOrder = EBinaryByteOrder::LittleEndian) -> void
		{
			using Unsigned = std::make_unsigned_t<T>;
			const Unsigned Encoded = static_cast<Unsigned>(Value);
			std::array<std::byte, sizeof(T)> Raw{};
			for (size_t Index = 0; Index < sizeof(T); ++Index)
			{
				const size_t ShiftIndex = ByteOrder == EBinaryByteOrder::LittleEndian
					? Index : sizeof(T) - Index - 1;
				Raw[Index] = static_cast<std::byte>(Encoded >> (ShiftIndex * 8));
			}
			WriteBytes(Raw);
		}

		CORE_API auto WriteI8(int8 Value) -> void;
		CORE_API auto WriteI16(int16 Value) -> void;
		CORE_API auto WriteU8(uint8 Value) -> void;
		CORE_API auto WriteU16(uint16 Value) -> void;
		CORE_API auto WriteU32(uint32 Value) -> void;
		CORE_API auto WriteU64(uint64 Value) -> void;
		CORE_API auto WriteI32(int32 Value) -> void;
		CORE_API auto WriteI64(int64 Value) -> void;
		CORE_API auto WriteFloat(float Value) -> void;
		CORE_API auto WriteDouble(double Value) -> void;
		CORE_API auto WriteVarUInt(uint64 Value) -> void;
		// Uses ZigZag followed by canonical unsigned VarInt encoding.
		CORE_API auto WriteVarInt(int64 Value) -> void;
		CORE_API auto WriteGuid(const FGuid& Value) -> void;
		CORE_API auto WriteHash128(const FXxHash128& Value) -> void;
		CORE_API auto WriteString(std::string_view Value) -> void;
		CORE_API auto WriteBytes(std::span<const std::byte> Value) -> void;
		CORE_API auto WriteHeader(const FBinaryFormatHeader& Header) -> void;
		auto GetBytes() const -> const FByteArray& { return Bytes; }
		CORE_API auto TakeBytes() -> FByteArray;
		auto Tell() const -> uint64 { return static_cast<uint64>(Bytes.size()); }
		auto HasError() const -> bool { return bLimitError || Archive.HasError(); }

	private:
		auto CanWrite(uint64 ByteCount, uint64 FieldBytes) -> bool
		{
			if (bLimitError || FieldBytes > Limits.MaximumFieldBytes
				|| Tell() > Limits.MaximumTotalBytes
				|| ByteCount > Limits.MaximumTotalBytes - Tell())
			{
				bLimitError = true;
				return false;
			}
			return true;
		}

		FByteArray Bytes;
		FBinaryCursorLimits Limits;
		bool bLimitError = false;
		FCanonicalMemoryWriter Archive;
	};

	// Reads bounded binary values without owning the source span.
	// An input above the configured total bound is unusable from construction.
	class FBinaryReader
	{
	public:
		explicit FBinaryReader(std::span<const std::byte> InBytes, FBinaryCursorLimits InLimits = {})
			: Limits(InLimits), bLimitError(static_cast<uint64>(InBytes.size()) > Limits.MaximumTotalBytes),
			  Archive(InBytes, EArchivePurpose::DerivedDataPayload) {}

		// Reads one fixed-width integer without publishing a truncated value.
		template<CBinaryInteger T>
		auto ReadInteger(T& Value, EBinaryByteOrder ByteOrder = EBinaryByteOrder::LittleEndian) -> bool
		{
			std::span<const std::byte> Raw;
			if (!ReadRegion(Raw, sizeof(T), sizeof(T))) return false;
			using Unsigned = std::make_unsigned_t<T>;
			Unsigned Encoded = 0;
			for (size_t Index = 0; Index < sizeof(T); ++Index)
			{
				const size_t ShiftIndex = ByteOrder == EBinaryByteOrder::LittleEndian
					? Index : sizeof(T) - Index - 1;
				Encoded |= static_cast<Unsigned>(std::to_integer<uint8>(Raw[Index])) << (ShiftIndex * 8);
			}
			if constexpr (std::is_signed_v<T>) Value = std::bit_cast<T>(Encoded);
			else Value = Encoded;
			return true;
		}

		CORE_API auto ReadI8(int8& Value) -> bool;
		CORE_API auto ReadI16(int16& Value) -> bool;
		CORE_API auto ReadU8(uint8& Value) -> bool;
		CORE_API auto ReadU16(uint16& Value) -> bool;
		CORE_API auto ReadU32(uint32& Value) -> bool;
		CORE_API auto ReadU64(uint64& Value) -> bool;
		CORE_API auto ReadI32(int32& Value) -> bool;
		CORE_API auto ReadI64(int64& Value) -> bool;
		CORE_API auto ReadFloat(float& Value) -> bool;
		CORE_API auto ReadDouble(double& Value) -> bool;
		// Rejects truncated, overflowing, and non-shortest unsigned encodings.
		CORE_API auto ReadVarUInt(uint64& Value) -> bool;
		CORE_API auto ReadVarInt(int64& Value) -> bool;
		CORE_API auto ReadGuid(FGuid& Value) -> bool;
		CORE_API auto ReadHash128(FXxHash128& Value) -> bool;
		CORE_API auto ReadString(std::string& Value, uint64 MaximumBytes = MaximumBinaryStringBytes) -> bool;
		CORE_API auto ReadBytes(FByteArray& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool;
		// Returns a non-owning region whose lifetime is bounded by the reader source.
		CORE_API auto ReadRegion(
			std::span<const std::byte>& Value, uint64 ByteCount, uint64 MaximumBytes) -> bool;
		CORE_API auto ReadAndValidateHeader(uint32 ExpectedMagic, uint32 ExpectedSchemaVersion,
			uint32 ExpectedFormatVersion, FBinaryFormatHeader* OutHeader = nullptr) -> bool;
		auto IsAtEnd() const -> bool { return !HasError() && Archive.GetRemainingPayloadBytes() == 0; }
		auto Tell() const -> uint64 { return Archive.Tell(); }
		auto HasError() const -> bool { return bLimitError || Archive.HasError(); }
		auto GetRemainingBytes() const -> size_t
		{
			return static_cast<size_t>(Archive.GetRemainingPayloadBytes());
		}

	private:
		FBinaryCursorLimits Limits;
		bool bLimitError = false;
		FCanonicalMemoryReader Archive;
	};

	// Reads one fixed-width unsigned little-endian value without advancing a reader.
	template<typename T>
	requires std::is_unsigned_v<T>
	[[nodiscard]] auto ReadLittleEndianAt(
		std::span<const std::byte> Bytes, uint64 Offset, T& OutValue) -> bool
	{
		if (Offset > static_cast<uint64>(Bytes.size())) return false;
		const size_t LocalOffset = static_cast<size_t>(Offset);
		if (sizeof(T) > Bytes.size() - LocalOffset) return false;

		T Value = 0;
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Value |= static_cast<T>(std::to_integer<uint8>(Bytes[LocalOffset + Index])) << (Index * 8);
		OutValue = Value;
		return true;
	}
} // namespace Durin
