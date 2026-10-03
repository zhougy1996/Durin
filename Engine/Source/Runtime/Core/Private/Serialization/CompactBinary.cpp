#include "Serialization/CompactBinary.h"
#include "Serialization/BinaryEncoding.h"

namespace Durin
{
	namespace
	{
		constexpr uint64 HeaderSize = 13; // type, name bytes, payload bytes
		struct FParts { ECbFieldType Type{}; uint32 NameSize = 0; uint64 PayloadSize = 0; uint64 PayloadOffset = 0; };

		template<typename T> auto Append(FByteBuffer& Out, T Value) -> void
		{
			const auto Encoded = EncodeBinaryInteger(Value);
			Out.insert(Out.end(), Encoded.begin(), Encoded.end());
		}
		auto Parse(FByteView Bytes, FParts& Out) -> bool
		{
			if (Bytes.size() < HeaderSize) return false;
			Out.Type = static_cast<ECbFieldType>(uint8(Bytes[0]));
			if (Out.Type > ECbFieldType::Object) return false;
			std::memcpy(&Out.NameSize, Bytes.data() + 1, 4);
			std::memcpy(&Out.PayloadSize, Bytes.data() + 5, 8);
			if constexpr (std::endian::native == std::endian::big)
			{
				Out.NameSize = std::byteswap(Out.NameSize); Out.PayloadSize = std::byteswap(Out.PayloadSize);
			}
			Out.PayloadOffset = HeaderSize + Out.NameSize;
			return Out.PayloadOffset <= Bytes.size() && Out.PayloadSize == Bytes.size() - Out.PayloadOffset;
		}
		auto IsUtf8(std::string_view Text) -> bool
		{
			const auto* P = reinterpret_cast<const unsigned char*>(Text.data());
			const auto* End = P + Text.size();
			while (P != End)
			{
				if (*P < 0x80) { if (*P == 0) return false; ++P; continue; }
				const unsigned char Lead = *P;
				uint32 Count = Lead >= 0xf0 ? 4 : Lead >= 0xe0 ? 3 : Lead >= 0xc2 ? 2 : 0;
				if (!Count || static_cast<size_t>(End - P) < Count) return false;
				for (uint32 I = 1; I < Count; ++I) if ((P[I] & 0xc0) != 0x80) return false;
				if ((Count == 3 && Lead == 0xe0 && P[1] < 0xa0)
					|| (Count == 3 && Lead == 0xed && P[1] >= 0xa0)
					|| (Count == 4 && Lead == 0xf0 && P[1] < 0x90)
					|| (Count == 4 && (Lead > 0xf4 || (Lead == 0xf4 && P[1] > 0x8f)))) return false;
				P += Count;
			}
			return true;
		}
		template<typename T> auto IsFiniteFloat(FByteView Payload) -> bool
		{
			T Bits{}; std::memcpy(&Bits, Payload.data(), sizeof(T));
			if constexpr (std::endian::native == std::endian::big) Bits = std::byteswap(Bits);
			if constexpr (sizeof(T) == 4) return std::isfinite(std::bit_cast<float>(Bits));
			else return std::isfinite(std::bit_cast<double>(Bits));
		}
		auto ValidateField(FByteView Bytes, const FCbLimits& Limits, uint32 Depth,
			uint32& FieldCount, bool RequireName, bool ForbidName) -> bool
		{
			FParts P;
			if (!Parse(Bytes, P) || Depth > Limits.MaximumDepth || ++FieldCount > Limits.MaximumFields) return false;
			const auto Name = std::string_view(reinterpret_cast<const char*>(Bytes.data() + HeaderSize), P.NameSize);
			if ((RequireName && Name.empty()) || (ForbidName && !Name.empty()) || Name.size() > Limits.MaximumStringBytes || !IsUtf8(Name)) return false;
			const auto Payload = Bytes.subspan(P.PayloadOffset, P.PayloadSize);
			switch (P.Type)
			{
			case ECbFieldType::Null: return Payload.empty();
			case ECbFieldType::Bool: return Payload.size() == 1 && uint8(Payload[0]) <= 1;
			case ECbFieldType::Int64: case ECbFieldType::UInt64: return Payload.size() == 8;
			case ECbFieldType::Float32: return Payload.size() == 4 && IsFiniteFloat<uint32>(Payload);
			case ECbFieldType::Float64: return Payload.size() == 8 && IsFiniteFloat<uint64>(Payload);
			case ECbFieldType::String: return Payload.size() <= Limits.MaximumStringBytes
				&& IsUtf8({reinterpret_cast<const char*>(Payload.data()), Payload.size()});
			case ECbFieldType::Binary: return Payload.size() <= Limits.MaximumBinaryBytes;
			case ECbFieldType::ObjectId: return Payload.size() == 12;
			case ECbFieldType::Array: case ECbFieldType::Object:
			{
				if (Payload.size() < 4) return false;
				uint32 Count; std::memcpy(&Count, Payload.data(), 4);
				if constexpr (std::endian::native == std::endian::big) Count = std::byteswap(Count);
				if (Count > Limits.MaximumFields) return false;
				uint64 Offset = 4; std::string_view Previous;
				for (uint32 I = 0; I < Count; ++I)
				{
					if (Offset > Payload.size() || Payload.size() - Offset < HeaderSize) return false;
					FParts Child; if (!Parse(Payload.subspan(Offset), Child))
					{
						// Parse requires an exact region; determine it from the header first.
						FByteView Tail = Payload.subspan(Offset);
						std::memcpy(&Child.NameSize, Tail.data() + 1, 4); std::memcpy(&Child.PayloadSize, Tail.data() + 5, 8);
						if constexpr (std::endian::native == std::endian::big) { Child.NameSize = std::byteswap(Child.NameSize); Child.PayloadSize = std::byteswap(Child.PayloadSize); }
					}
					const uint64 Size = HeaderSize + Child.NameSize + Child.PayloadSize;
					if (Size > Payload.size() - Offset) return false;
					const auto ChildBytes = Payload.subspan(Offset, Size);
					if (!ValidateField(ChildBytes, Limits, Depth + 1, FieldCount,
						P.Type == ECbFieldType::Object, P.Type == ECbFieldType::Array)) return false;
					if (P.Type == ECbFieldType::Object)
					{
						FParts CP; Parse(ChildBytes, CP);
						const std::string_view Current(reinterpret_cast<const char*>(ChildBytes.data() + HeaderSize), CP.NameSize);
						if (!Previous.empty() && Previous >= Current) return false;
						Previous = Current;
					}
					Offset += Size;
				}
				return Offset == Payload.size();
			}
			}
			return false;
		}
		auto Children(const FCbFieldView& Field) -> std::vector<FCbFieldView>
		{
			std::vector<FCbFieldView> Result;
			FParts P; if (!Parse(Field.GetBytes().GetBytes(), P) || P.PayloadSize < 4) return Result;
			const auto Payload = Field.GetBytes().MakeView(P.PayloadOffset, P.PayloadSize);
			uint32 Count; std::memcpy(&Count, Payload.data(), 4);
			if constexpr (std::endian::native == std::endian::big) Count = std::byteswap(Count);
			uint64 Offset = 4; Result.reserve(Count);
			for (uint32 I = 0; I < Count; ++I)
			{
				FByteView Tail = Payload.GetBytes().subspan(Offset); FParts C{};
				std::memcpy(&C.NameSize, Tail.data() + 1, 4); std::memcpy(&C.PayloadSize, Tail.data() + 5, 8);
				if constexpr (std::endian::native == std::endian::big) { C.NameSize = std::byteswap(C.NameSize); C.PayloadSize = std::byteswap(C.PayloadSize); }
				const uint64 Size = HeaderSize + C.NameSize + C.PayloadSize;
				auto Child = FCbField::TryLoad(Payload.MakeView(Offset, Size));
				if (!Child) return {};
				Result.push_back(Child->GetView()); Offset += Size;
			}
			return Result;
		}
	}

	auto FCbField::TryLoad(FSharedByteBuffer Bytes, FCbLimits Limits) -> std::expected<FCbField, std::string>
	{
		if (Bytes.IsEmpty() || Bytes.GetSize() > Limits.MaximumEncodedBytes) return std::unexpected("Compact Binary byte limit exceeded.");
		uint32 Count = 0;
		if (!ValidateField(Bytes.GetBytes(), Limits, 0, Count, false, false)) return std::unexpected("Compact Binary field is malformed or non-canonical.");
		return FCbField(std::move(Bytes));
	}
	auto FCbObject::TryLoad(FSharedByteBuffer Bytes, FCbLimits Limits) -> std::expected<FCbObject, std::string>
	{
		auto Field = FCbField::TryLoad(std::move(Bytes), Limits);
		if (!Field || Field->GetView().GetType() != ECbFieldType::Object) return std::unexpected("Compact Binary value is not an object.");
		return FCbObject(std::move(*Field));
	}
	auto FCbArray::TryLoad(FSharedByteBuffer Bytes, FCbLimits Limits) -> std::expected<FCbArray, std::string>
	{
		auto Field = FCbField::TryLoad(std::move(Bytes), Limits);
		if (!Field || Field->GetView().GetType() != ECbFieldType::Array) return std::unexpected("Compact Binary value is not an array.");
		return FCbArray(std::move(*Field));
	}
	auto FCbField::GetView() const -> FCbFieldView { return FCbFieldView(Bytes); }
	auto FCbObject::GetView() const -> FCbObjectView { return FCbObjectView(Field.GetView()); }
	auto FCbArray::GetView() const -> FCbArrayView { return FCbArrayView(Field.GetView()); }
	auto FCbFieldView::GetType() const -> ECbFieldType { FParts P; return Parse(Bytes.GetBytes(), P) ? P.Type : ECbFieldType::Null; }
	auto FCbFieldView::GetName() const -> std::string_view { FParts P; return Parse(Bytes.GetBytes(), P) ? std::string_view(reinterpret_cast<const char*>(Bytes.data() + HeaderSize), P.NameSize) : std::string_view{}; }
	template<typename T> static auto Scalar(const FSharedByteBuffer& Bytes, ECbFieldType Type) -> std::optional<T>
	{
		FParts P; if (!Parse(Bytes.GetBytes(), P) || P.Type != Type || P.PayloadSize != sizeof(T)) return {};
		T V; std::memcpy(&V, Bytes.data() + P.PayloadOffset, sizeof(T));
		if constexpr (std::endian::native == std::endian::big && std::is_integral_v<T>) V = std::byteswap(V);
		return V;
	}
	auto FCbFieldView::AsBool() const -> std::optional<bool> { auto V = Scalar<uint8>(Bytes, ECbFieldType::Bool); return V ? std::optional<bool>(*V != 0) : std::nullopt; }
	auto FCbFieldView::AsInt64() const -> std::optional<int64> { return Scalar<int64>(Bytes, ECbFieldType::Int64); }
	auto FCbFieldView::AsUInt64() const -> std::optional<uint64> { return Scalar<uint64>(Bytes, ECbFieldType::UInt64); }
	auto FCbFieldView::AsFloat32() const -> std::optional<float> { auto V = Scalar<uint32>(Bytes, ECbFieldType::Float32); return V ? std::optional<float>(std::bit_cast<float>(*V)) : std::nullopt; }
	auto FCbFieldView::AsFloat64() const -> std::optional<double> { auto V = Scalar<uint64>(Bytes, ECbFieldType::Float64); return V ? std::optional<double>(std::bit_cast<double>(*V)) : std::nullopt; }
	auto FCbFieldView::AsString() const -> std::optional<std::string_view> { FParts P; if (!Parse(Bytes.GetBytes(), P) || P.Type != ECbFieldType::String) return {}; return std::string_view(reinterpret_cast<const char*>(Bytes.data() + P.PayloadOffset), P.PayloadSize); }
	auto FCbFieldView::AsBinary() const -> std::optional<FSharedByteBuffer> { FParts P; if (!Parse(Bytes.GetBytes(), P) || P.Type != ECbFieldType::Binary) return {}; return Bytes.MakeView(P.PayloadOffset, P.PayloadSize); }
	auto FCbFieldView::AsObjectId() const -> std::optional<FCbObjectId> { FParts P; if (!Parse(Bytes.GetBytes(), P) || P.Type != ECbFieldType::ObjectId) return {}; FCbObjectId V; std::memcpy(V.Bytes.data(), Bytes.data() + P.PayloadOffset, 12); return V; }
	auto FCbFieldView::AsObject() const -> FCbObjectView { return GetType() == ECbFieldType::Object ? FCbObjectView(*this) : FCbObjectView{}; }
	auto FCbFieldView::AsArray() const -> FCbArrayView { return GetType() == ECbFieldType::Array ? FCbArrayView(*this) : FCbArrayView{}; }
	auto FCbObjectView::GetFields() const -> std::vector<FCbFieldView> { return IsValid() ? Children(Field) : std::vector<FCbFieldView>{}; }
	auto FCbArrayView::GetFields() const -> std::vector<FCbFieldView> { return IsValid() ? Children(Field) : std::vector<FCbFieldView>{}; }
	auto FCbObjectView::Num() const -> uint32 { return static_cast<uint32>(GetFields().size()); }
	auto FCbArrayView::Num() const -> uint32 { return static_cast<uint32>(GetFields().size()); }
	auto FCbObjectView::Find(std::string_view Name) const -> FCbFieldView { auto Fields = GetFields(); const auto It = std::ranges::lower_bound(Fields, Name, {}, &FCbFieldView::GetName); return It != Fields.end() && It->GetName() == Name ? *It : FCbFieldView{}; }
	auto FCbArrayView::At(uint32 Index) const -> FCbFieldView { auto Fields = GetFields(); return Index < Fields.size() ? Fields[Index] : FCbFieldView{}; }

	auto FCbWriter::Add(ECbFieldType Type, FByteView Payload, std::string Name) -> bool
	{
		if (!Error.empty()) return false;
		if (Entries.size() >= Limits.MaximumFields || Name.size() > Limits.MaximumStringBytes
			|| !IsUtf8(Name) || Payload.size() > Limits.MaximumEncodedBytes)
		{ Error = "Compact Binary writer limit exceeded."; return false; }
		FEntry Entry{std::move(Name), {}}; Entry.Bytes.reserve(HeaderSize + Entry.Name.size() + Payload.size());
		Entry.Bytes.push_back(std::byte(Type)); Append(Entry.Bytes, uint32(Entry.Name.size())); Append(Entry.Bytes, uint64(Payload.size()));
		Entry.Bytes.insert(Entry.Bytes.end(), std::as_bytes(std::span(Entry.Name)).begin(), std::as_bytes(std::span(Entry.Name)).end());
		Entry.Bytes.insert(Entry.Bytes.end(), Payload.begin(), Payload.end()); Entries.push_back(std::move(Entry)); return true;
	}
	auto FCbWriter::AddNull(std::string N) -> bool { return Add(ECbFieldType::Null, {}, std::move(N)); }
	auto FCbWriter::AddBool(bool V, std::string N) -> bool { const uint8 X = V; return Add(ECbFieldType::Bool, std::as_bytes(std::span(&X, 1)), std::move(N)); }
	auto FCbWriter::AddInt64(int64 V, std::string N) -> bool { const auto X = EncodeBinaryInteger(V); return Add(ECbFieldType::Int64, X, std::move(N)); }
	auto FCbWriter::AddUInt64(uint64 V, std::string N) -> bool { const auto X = EncodeBinaryInteger(V); return Add(ECbFieldType::UInt64, X, std::move(N)); }
	auto FCbWriter::AddFloat32(float V, std::string N) -> bool { if (!std::isfinite(V)) { Error = "Compact Binary floats must be finite."; return false; } const auto X = EncodeBinaryInteger(std::bit_cast<uint32>(V)); return Add(ECbFieldType::Float32, X, std::move(N)); }
	auto FCbWriter::AddFloat64(double V, std::string N) -> bool { if (!std::isfinite(V)) { Error = "Compact Binary floats must be finite."; return false; } const auto X = EncodeBinaryInteger(std::bit_cast<uint64>(V)); return Add(ECbFieldType::Float64, X, std::move(N)); }
	auto FCbWriter::AddString(std::string V, std::string N) -> bool { if (V.size() > Limits.MaximumStringBytes || !IsUtf8(V)) { Error = "Compact Binary string is invalid or exceeds its limit."; return false; } return Add(ECbFieldType::String, std::as_bytes(std::span(V)), std::move(N)); }
	auto FCbWriter::AddBinary(FSharedByteBuffer V, std::string N) -> bool { if (V.GetSize() > Limits.MaximumBinaryBytes) { Error = "Compact Binary binary limit exceeded."; return false; } return Add(ECbFieldType::Binary, V.GetBytes(), std::move(N)); }
	auto FCbWriter::AddObjectId(FCbObjectId V, std::string N) -> bool { return Add(ECbFieldType::ObjectId, std::as_bytes(std::span(V.Bytes)), std::move(N)); }
	auto FCbWriter::AddObject(FCbObject V, std::string N) -> bool { FParts P; if (!Parse(V.GetBytes().GetBytes(), P) || P.Type != ECbFieldType::Object) return false; return Add(P.Type, V.GetBytes().GetBytes().subspan(P.PayloadOffset), std::move(N)); }
	auto FCbWriter::AddArray(FCbArray V, std::string N) -> bool { FParts P; if (!Parse(V.GetBytes().GetBytes(), P) || P.Type != ECbFieldType::Array) return false; return Add(P.Type, V.GetBytes().GetBytes().subspan(P.PayloadOffset), std::move(N)); }
	auto FCbWriter::Save(bool Object) -> std::expected<FCbField, std::string>
	{
		if (!Error.empty()) return std::unexpected(Error);
		if (Object)
		{
			for (const auto& E : Entries) if (E.Name.empty()) return std::unexpected("Compact Binary object fields require names.");
			std::ranges::sort(Entries, {}, &FCbWriter::FEntry::Name);
			for (size_t I = 1; I < Entries.size(); ++I) if (Entries[I - 1].Name == Entries[I].Name) return std::unexpected("Compact Binary object field names must be unique.");
		}
		else for (const auto& E : Entries) if (!E.Name.empty()) return std::unexpected("Compact Binary array fields cannot have names.");
		FByteBuffer Payload; Append(Payload, uint32(Entries.size()));
		for (const auto& E : Entries) Payload.insert(Payload.end(), E.Bytes.begin(), E.Bytes.end());
		FByteBuffer Bytes; Bytes.push_back(std::byte(Object ? ECbFieldType::Object : ECbFieldType::Array)); Append(Bytes, uint32(0)); Append(Bytes, uint64(Payload.size())); Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
		if (Bytes.size() > Limits.MaximumEncodedBytes) return std::unexpected("Compact Binary encoded byte limit exceeded.");
		auto Loaded = FCbField::TryLoad(FSharedByteBuffer::Take(std::move(Bytes)), Limits);
		if (!Loaded) return std::unexpected(std::move(Loaded.error())); return Loaded;
	}
	auto FCbWriter::SaveObject() -> std::expected<FCbObject, std::string> { auto F = Save(true); if (!F) return std::unexpected(std::move(F.error())); return FCbObject(std::move(*F)); }
	auto FCbWriter::SaveArray() -> std::expected<FCbArray, std::string> { auto F = Save(false); if (!F) return std::unexpected(std::move(F.error())); return FCbArray(std::move(*F)); }
}
