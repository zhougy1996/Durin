#pragma once

namespace Durin::EngineWire
{
	inline auto AlignUp(uint64 Value, uint32 Alignment) -> uint64
	{
		check(Alignment != 0 && (Alignment & (Alignment - 1)) == 0);
		const uint64 Mask = static_cast<uint64>(Alignment) - 1;
		check(Value <= std::numeric_limits<uint64>::max() - Mask);
		return (Value + Mask) & ~Mask;
	}

	template<typename T>
	requires std::is_unsigned_v<T>
	auto ReadLittleEndianAt(
		std::span<const uint8> Bytes, uint64 Offset, T& OutValue) -> bool
	{
		if (Offset > Bytes.size() || Bytes.size() - static_cast<size_t>(Offset) < sizeof(T))
			return false;
		OutValue = 0;
		for (uint32 Index = 0; Index < sizeof(T); ++Index)
			OutValue |= static_cast<T>(Bytes[static_cast<size_t>(Offset) + Index]) << (Index * 8);
		return true;
	}

	class FWriter
	{
	public:
		auto WriteU8(uint8 Value) -> void { Bytes.push_back(Value); }
		auto WriteU16(uint16 Value) -> void { WriteUnsigned(Value); }
		auto WriteU32(uint32 Value) -> void { WriteUnsigned(Value); }
		auto WriteU64(uint64 Value) -> void { WriteUnsigned(Value); }
		auto WriteFloat(float Value) -> void { WriteU32(std::bit_cast<uint32>(Value)); }
		auto WriteBytes(std::span<const uint8> Value) -> void
		{
			Bytes.insert(Bytes.end(), Value.begin(), Value.end());
		}
		auto WriteZeroes(size_t Count) -> void { Bytes.insert(Bytes.end(), Count, 0); }
		auto WriteString(std::string_view Value) -> void
		{
			check(Value.size() <= std::numeric_limits<uint32>::max());
			WriteU32(static_cast<uint32>(Value.size()));
			Bytes.insert(Bytes.end(), Value.begin(), Value.end());
		}
		auto GetBytes() const -> const std::vector<uint8>& { return Bytes; }
		auto TakeBytes() -> std::vector<uint8> { return std::move(Bytes); }

	private:
		template<typename T>
		requires std::is_unsigned_v<T>
		auto WriteUnsigned(T Value) -> void
		{
			for (uint32 Index = 0; Index < sizeof(T); ++Index)
				WriteU8(static_cast<uint8>(Value >> (Index * 8)));
		}

		std::vector<uint8> Bytes;
	};

	class FReader
	{
	public:
		explicit FReader(std::span<const uint8> InBytes) : Bytes(InBytes) {}

		auto ReadU8(uint8& Value) -> bool
		{
			if (Offset >= Bytes.size()) return false;
			Value = Bytes[Offset++];
			return true;
		}
		auto ReadU16(uint16& Value) -> bool { return ReadUnsigned(Value); }
		auto ReadU32(uint32& Value) -> bool { return ReadUnsigned(Value); }
		auto ReadU64(uint64& Value) -> bool { return ReadUnsigned(Value); }
		auto ReadFloat(float& Value) -> bool
		{
			uint32 Bits = 0;
			if (!ReadU32(Bits)) return false;
			Value = std::bit_cast<float>(Bits);
			return true;
		}
		auto ReadString(std::string& Value, size_t MaximumBytes) -> bool
		{
			uint32 Count = 0;
			if (!ReadU32(Count) || Count == 0 || Count > MaximumBytes || Count > Remaining())
				return false;
			Value.assign(reinterpret_cast<const char*>(Bytes.data() + Offset), Count);
			Offset += Count;
			return Value.find('\0') == std::string::npos;
		}
		auto IsAtEnd() const -> bool { return Offset == Bytes.size(); }
		auto AtEnd() const -> bool { return IsAtEnd(); }
		auto GetRemainingBytes() const -> size_t { return Bytes.size() - Offset; }
		auto Remaining() const -> size_t { return GetRemainingBytes(); }

	private:
		template<typename T>
		requires std::is_unsigned_v<T>
		auto ReadUnsigned(T& Value) -> bool
		{
			if (Remaining() < sizeof(T)) return false;
			Value = 0;
			for (uint32 Index = 0; Index < sizeof(T); ++Index)
				Value |= static_cast<T>(Bytes[Offset++]) << (Index * 8);
			return true;
		}

		std::span<const uint8> Bytes;
		size_t Offset = 0;
	};
}
