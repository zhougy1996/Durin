#include "Serialization/Archive.h"

namespace Durin
{
	namespace
	{
		auto IsPersistentPurpose(EArchivePurpose Purpose) -> bool
		{
			return Purpose == EArchivePurpose::AuthoredPackage
				|| Purpose == EArchivePurpose::DerivedDataKey
				|| Purpose == EArchivePurpose::DerivedDataPayload
				|| Purpose == EArchivePurpose::CookedPackage
				|| Purpose == EArchivePurpose::CookedPayload
				|| Purpose == EArchivePurpose::BulkData;
		}

		template<typename T>
		auto SerializeCanonicalInteger(FArchive& Ar, T& Value) -> FArchive&
		{
			using Unsigned = std::make_unsigned_t<T>;
			std::array<std::byte, sizeof(T)> Bytes{};
			if (Ar.IsSaving())
			{
				const Unsigned Bits = static_cast<Unsigned>(Value);
				for (size_t Index = 0; Index < sizeof(T); ++Index)
					Bytes[Index] = static_cast<std::byte>((Bits >> (Index * 8)) & 0xffu);
			}
			Ar.SerializeRawBytes(Bytes);
			if (Ar.IsLoading() && !Ar.HasError())
			{
				Unsigned Bits = 0;
				for (size_t Index = 0; Index < sizeof(T); ++Index)
					Bits |= static_cast<Unsigned>(std::to_integer<uint8>(Bytes[Index])) << (Index * 8);
				Value = static_cast<T>(Bits);
			}
			return Ar;
		}

		template<typename T, typename U>
		auto SerializeCanonicalFloat(FArchive& Ar, T& Value) -> FArchive&
		{
			U Bits = Ar.IsSaving() ? std::bit_cast<U>(Value) : U{};
			SerializeCanonicalInteger(Ar, Bits);
			if (Ar.IsLoading() && !Ar.HasError()) Value = std::bit_cast<T>(Bits);
			return Ar;
		}

		auto MakeMemoryState(EArchiveDirection Direction, EArchivePurpose Purpose, FArchiveState Context) -> FArchiveState
		{
			Context.Direction = Direction;
			Context.Purpose = Purpose;
			Context.Capabilities |= EArchiveCapability::RawBytes | EArchiveCapability::Position;
			if (Direction == EArchiveDirection::Load)
				Context.Capabilities |= EArchiveCapability::RemainingPayload;
			Context.bPersistent = Context.bPersistent || IsPersistentPurpose(Purpose);
			Context.bCooking = Context.bCooking || Purpose == EArchivePurpose::CookedPackage
				|| Purpose == EArchivePurpose::CookedPayload;
			return Context;
		}
	}

	auto FArchiveVersionContext::FindFormat(FName Format) const -> const FArchiveFormatVersion*
	{
		const auto It = std::ranges::find(Formats, Format, &FArchiveFormatVersion::Format);
		return It == Formats.end() ? nullptr : &*It;
	}

	auto FArchiveVersionContext::FindCustom(const FGuid& Key) const -> const FArchiveCustomVersion*
	{
		const auto It = std::ranges::find(CustomVersions, Key, &FArchiveCustomVersion::Key);
		return It == CustomVersions.end() ? nullptr : &*It;
	}

	FArchive::FArchive(FArchiveState InState, FArchiveVersionContext InVersions)
		: State(std::move(InState)), Versions(std::move(InVersions))
	{
		State.bPersistent = State.bPersistent || IsPersistentPurpose(State.Purpose);
		State.bCooking = State.bCooking || State.Purpose == EArchivePurpose::CookedPackage
			|| State.Purpose == EArchivePurpose::CookedPayload;
	}

	auto FArchive::PushPath(std::string Segment) -> void { PathSegments.push_back(std::move(Segment)); }
	auto FArchive::PopPath() -> void { if (!PathSegments.empty()) PathSegments.pop_back(); }
	auto FArchive::GetPathString() const -> std::string
	{
		std::string Path;
		for (const std::string& Segment : PathSegments) Path += Segment;
		return Path;
	}

	auto FArchive::Fail(EArchiveFailureCode Code, std::string_view Message) -> void
	{
		if (Failure) return;
		Failure = std::make_unique<FArchiveFailure>(FArchiveFailure{
			Code, GetPathString(), std::string(Message)});
	}

	auto FArchive::FormatFailure() const -> std::string
	{
		if (!Failure) return {};
		static constexpr std::string_view Names[] = {
			"UnsupportedCapability", "UnsupportedType", "InvalidData", "TruncatedPayload",
			"UnbalancedScope", "MissingBaseReflectedFields", "DuplicateBaseReflectedFields",
			"DuplicateField", "MalformedSerializer", "InvalidObjectReference", "InvalidPath",
			"UnsupportedVersion", "Overflow", "LimitExceeded", "InvalidAlignment",
			"NonZeroPadding", "TrailingData"};
		return std::format("ArchiveFailure:{}:{}: {}", Names[static_cast<size_t>(Failure->Code)],
			Failure->Path, Failure->Message);
	}

	auto FArchive::GetError() const -> std::string_view
	{
		FormattedFailure = FormatFailure();
		return FormattedFailure;
	}

	auto FArchive::SerializeRawBytes(std::span<std::byte>) -> void
	{
		Fail(EArchiveFailureCode::UnsupportedCapability, "This Archive does not support RawBytes.");
	}

	auto FArchive::Serialize(void* Data, uint64 Size) -> void
	{
		if (Size > static_cast<uint64>(std::numeric_limits<size_t>::max()))
		{
			Fail(EArchiveFailureCode::Overflow, "Raw byte size exceeds the addressable span range.");
			return;
		}
		if (Size != 0 && Data == nullptr)
		{
			Fail(EArchiveFailureCode::InvalidData, "A nonempty raw byte transfer requires storage.");
			return;
		}
		SerializeRawBytes({static_cast<std::byte*>(Data), static_cast<size_t>(Size)});
	}

	auto FArchive::WriteBytes(std::span<const std::byte> Bytes) -> void
	{
		if (!IsSaving())
		{
			Fail(EArchiveFailureCode::InvalidData, "WriteBytes requires a saving Archive.");
			return;
		}
		Serialize(const_cast<std::byte*>(Bytes.data()), static_cast<uint64>(Bytes.size()));
	}

	auto FArchive::ReadBytes(std::span<std::byte> Bytes) -> void
	{
		if (!IsLoading())
		{
			Fail(EArchiveFailureCode::InvalidData, "ReadBytes requires a loading Archive.");
			return;
		}
		Serialize(Bytes.data(), static_cast<uint64>(Bytes.size()));
	}

	auto FArchive::SerializeByteBlob(std::vector<std::byte>& Bytes) -> void
	{
		constexpr uint64 MaximumBlobBytes = 1024ull * 1024 * 1024;
		uint64 Size = IsSaving() ? static_cast<uint64>(Bytes.size()) : 0;
		std::array<std::byte, sizeof(uint64)> EncodedSize{};
		if (IsSaving())
			for (size_t Index = 0; Index < EncodedSize.size(); ++Index)
				EncodedSize[Index] = static_cast<std::byte>((Size >> (Index * 8)) & 0xffu);
		SerializeRawBytes(EncodedSize);
		if (HasError()) return;
		if (IsLoading())
			for (size_t Index = 0; Index < EncodedSize.size(); ++Index)
				Size |= static_cast<uint64>(std::to_integer<uint8>(EncodedSize[Index])) << (Index * 8);
		if (Size > MaximumBlobBytes
			|| Size > static_cast<uint64>(std::numeric_limits<size_t>::max())
			|| (IsLoading() && Size > GetRemainingPayloadBytes()))
		{
			Fail(EArchiveFailureCode::LimitExceeded,
				"Byte Blob is truncated or exceeds the 1 GiB allocation limit.");
			return;
		}
		if (IsSaving())
		{
			WriteBytes(Bytes);
			return;
		}
		std::vector<std::byte> Candidate(static_cast<size_t>(Size));
		ReadBytes(Candidate);
		if (!HasError()) Bytes = std::move(Candidate);
	}

	auto FArchive::SerializeBulkData(FArchiveBulkDataTransfer& Value) -> void
	{
		if (GetBulkDataPolicy() == EArchiveBulkDataPolicy::Skip) return;
		if (GetBulkDataPolicy() == EArchiveBulkDataPolicy::External)
		{
			Fail(EArchiveFailureCode::UnsupportedCapability,
				"External bulk data requires an Archive-owned payload adapter.");
			return;
		}

		FGuid PayloadId = Value.PayloadId;
		FGuid FormatId = Value.FormatId;
		uint32 FormatVersion = Value.FormatVersion;
		uint64 LogicalSize = Value.LogicalSize;
		uint64 StoredSize = Value.StoredSize;
		uint64 HashLow = Value.ContentHash.HashLow;
		uint64 HashHigh = Value.ContentHash.HashHigh;
		uint64 ContainerHashLow = 0;
		uint64 ContainerHashHigh = 0;
		uint8 StorageKind = static_cast<uint8>(EArchiveBulkDataStorageKind::Inline);
		*this << StorageKind << PayloadId << FormatId << FormatVersion
			<< LogicalSize << StoredSize << HashLow << HashHigh
			<< ContainerHashLow << ContainerHashHigh;
		if (HasError()) return;
		if (IsLoading() && StorageKind != static_cast<uint8>(EArchiveBulkDataStorageKind::Inline))
		{
			Fail(EArchiveFailureCode::UnsupportedCapability,
				"Inline Archive encountered an externally stored bulk payload.");
			return;
		}

		if (IsSaving())
		{
			if (Value.Residency != EArchiveBulkDataResidency::Resident)
			{
				Fail(EArchiveFailureCode::InvalidData,
					"Inline bulk serialization requires verified resident bytes.");
				return;
			}
			const std::span<const std::byte> Bytes = Value.Buffer.GetBytes();
			if (LogicalSize != Bytes.size() || StoredSize != Bytes.size()
				|| FXxHash128::HashBuffer(Bytes) != Value.ContentHash)
			{
				Fail(EArchiveFailureCode::InvalidData,
					"Inline bulk descriptor size or content hash does not match its resident bytes.");
				return;
			}
			std::vector<std::byte> Candidate(Bytes.begin(), Bytes.end());
			SerializeByteBlob(Candidate);
			return;
		}

		std::vector<std::byte> Candidate;
		SerializeByteBlob(Candidate);
		if (HasError()) return;
		if (LogicalSize != Candidate.size() || StoredSize != Candidate.size()
			|| FXxHash128::HashBuffer(Candidate) != FXxHash128{HashLow, HashHigh})
		{
			Fail(EArchiveFailureCode::InvalidData,
				"Inline bulk payload size or content hash verification failed.");
			return;
		}
		Value = {
			.PayloadId = PayloadId,
			.FormatId = FormatId,
			.FormatVersion = FormatVersion,
			.LogicalSize = LogicalSize,
			.StoredSize = StoredSize,
			.ContentHash = {HashLow, HashHigh},
			.ContainerHash = {ContainerHashLow, ContainerHashHigh},
			.StorageKind = EArchiveBulkDataStorageKind::Inline,
			.Residency = EArchiveBulkDataResidency::Resident,
			.Buffer = FSharedByteBuffer::Take(std::move(Candidate))};
	}

	auto FArchive::TryCaptureLogicalPrimitive(EArchiveLogicalPrimitiveKind, const void*) -> bool { return false; }
	auto FArchive::TryCaptureLogicalText(EArchiveLogicalTextKind, std::string_view) -> bool { return false; }

	auto FArchive::operator<<(bool& Value) -> FArchive&
	{
		if (!IsCurrentFieldAvailable()) return *this;
		if (IsSaving() && TryCaptureLogicalPrimitive(EArchiveLogicalPrimitiveKind::Bool, &Value)) return *this;
		uint8 Encoded = IsSaving() && Value ? 1 : 0;
		SerializeCanonicalInteger(*this, Encoded);
		if (IsLoading() && !HasError())
		{
			if (Encoded > 1) Fail(EArchiveFailureCode::InvalidData, "Boolean encoding must be zero or one.");
			else Value = Encoded != 0;
		}
		return *this;
	}

#define DURIN_ARCHIVE_INTEGER(Type, Kind) auto FArchive::operator<<(Type& Value) -> FArchive& \
	{ \
		if (!IsCurrentFieldAvailable()) return *this; \
		if (IsSaving() && TryCaptureLogicalPrimitive(EArchiveLogicalPrimitiveKind::Kind, &Value)) return *this; \
		return SerializeCanonicalInteger(*this, Value); \
	}
	DURIN_ARCHIVE_INTEGER(int8, Int8)
	DURIN_ARCHIVE_INTEGER(int16, Int16)
	DURIN_ARCHIVE_INTEGER(int32, Int32)
	DURIN_ARCHIVE_INTEGER(int64, Int64)
	DURIN_ARCHIVE_INTEGER(uint8, UInt8)
	DURIN_ARCHIVE_INTEGER(uint16, UInt16)
	DURIN_ARCHIVE_INTEGER(uint32, UInt32)
	DURIN_ARCHIVE_INTEGER(uint64, UInt64)
#undef DURIN_ARCHIVE_INTEGER

	auto FArchive::operator<<(float& Value) -> FArchive&
	{
		if (!IsCurrentFieldAvailable()) return *this;
		if (IsSaving() && TryCaptureLogicalPrimitive(EArchiveLogicalPrimitiveKind::Float32, &Value)) return *this;
		return SerializeCanonicalFloat<float, uint32>(*this, Value);
	}
	auto FArchive::operator<<(double& Value) -> FArchive&
	{
		if (!IsCurrentFieldAvailable()) return *this;
		if (IsSaving() && TryCaptureLogicalPrimitive(EArchiveLogicalPrimitiveKind::Float64, &Value)) return *this;
		return SerializeCanonicalFloat<double, uint64>(*this, Value);
	}
	auto FArchive::operator<<(FName& Value) -> FArchive&
	{
		if (!IsCurrentFieldAvailable()) return *this;
		if (IsSaving() && TryCaptureLogicalText(EArchiveLogicalTextKind::Name, Value.ToString())) return *this;
		std::string Text = IsSaving() ? Value.ToString() : std::string();
		*this << Text;
		if (IsLoading() && !HasError()) Value = FName(Text);
		return *this;
	}
	auto FArchive::operator<<(FGuid& Value) -> FArchive&
	{
		if (IsSaving() && TryCaptureLogicalPrimitive(EArchiveLogicalPrimitiveKind::Guid, &Value)) return *this;
		return *this << Value.A << Value.B << Value.C << Value.D;
	}
	auto FArchive::operator<<(std::string& Value) -> FArchive&
	{
		return SerializeBoundedString(*this, Value, std::numeric_limits<uint64>::max()), *this;
	}

	FCanonicalMemoryWriter::FCanonicalMemoryWriter(
		std::vector<uint8>& InBytes, EArchivePurpose Purpose, FArchiveState Context,
		FArchiveVersionContext Versions)
		: FArchive(MakeMemoryState(EArchiveDirection::Save, Purpose, std::move(Context)),
			std::move(Versions)), Bytes(InBytes)
	{
	}

	auto FCanonicalMemoryWriter::SerializeRawBytes(std::span<std::byte> Data) -> void
	{
		if (HasError()) return;
		if (Data.empty()) return;
		const auto* Source = reinterpret_cast<const uint8*>(Data.data());
		Bytes.insert(Bytes.end(), Source, Source + Data.size());
	}

	FCanonicalMemoryReader::FCanonicalMemoryReader(
		std::span<const uint8> InBytes, EArchivePurpose Purpose, FArchiveState Context,
		FArchiveVersionContext Versions)
		: FArchive(MakeMemoryState(EArchiveDirection::Load, Purpose, std::move(Context)),
			std::move(Versions)), Bytes(InBytes)
	{
	}

	auto FCanonicalMemoryReader::SerializeRawBytes(std::span<std::byte> Data) -> void
	{
		if (HasError()) return;
		if (Data.size() > GetRemainingPayloadBytes())
		{
			Fail(EArchiveFailureCode::TruncatedPayload, "Truncated byte payload.");
			return;
		}
		if (!Data.empty()) std::memcpy(Data.data(), Bytes.data() + Offset, Data.size());
		Offset += static_cast<uint64>(Data.size());
	}

	auto FCanonicalMemoryReader::ReadRegion(
		uint64 Size, std::span<const uint8>& OutRegion) -> bool
	{
		OutRegion = {};
		if (HasError()) return false;
		if (Size > GetRemainingPayloadBytes())
		{
			Fail(EArchiveFailureCode::TruncatedPayload, "Bounded Archive region is truncated.");
			return false;
		}
		OutRegion = Bytes.subspan(static_cast<size_t>(Offset), static_cast<size_t>(Size));
		Offset += Size;
		return true;
	}

	FCountingArchive::FCountingArchive(EArchivePurpose Purpose)
		: FArchive(MakeMemoryState(EArchiveDirection::Save, Purpose, {}))
	{
	}

	auto FCountingArchive::SerializeRawBytes(std::span<std::byte> Bytes) -> void
	{
		if (HasError()) return;
		if (Bytes.size() > std::numeric_limits<uint64>::max() - Count)
		{
			Fail(EArchiveFailureCode::Overflow, "Counting Archive byte extent overflowed.");
			return;
		}
		Count += static_cast<uint64>(Bytes.size());
	}

	FHashingArchive::FHashingArchive(EArchivePurpose Purpose)
		: FArchive(MakeMemoryState(EArchiveDirection::Save, Purpose, {}))
	{
	}

	auto FHashingArchive::SerializeRawBytes(std::span<std::byte> Bytes) -> void
	{
		if (HasError()) return;
		if (Bytes.size() > std::numeric_limits<uint64>::max() - Count)
		{
			Fail(EArchiveFailureCode::Overflow, "Hashing Archive byte extent overflowed.");
			return;
		}
		Builder.Update(Bytes);
		Count += static_cast<uint64>(Bytes.size());
	}

	auto SerializeByteBuffer(FArchive& Ar, std::vector<uint8>& Value, uint64 MaximumBytes) -> void
	{
		uint64 Size = Ar.IsSaving() ? static_cast<uint64>(Value.size()) : 0;
		Ar << Size;
		if (Ar.HasError()) return;
		if (Size > MaximumBytes || Size > static_cast<uint64>(std::vector<uint8>().max_size()))
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded, "Byte buffer exceeds its serialization limit.");
			return;
		}
		if (Ar.IsLoading())
		{
			if (Size > Ar.GetRemainingPayloadBytes())
			{
				Ar.Fail(EArchiveFailureCode::TruncatedPayload, "Byte buffer is truncated.");
				return;
			}
			std::vector<uint8> Loaded(static_cast<size_t>(Size));
			if (Size != 0) Ar.ReadBytes(std::as_writable_bytes(std::span<uint8>(Loaded)));
			if (!Ar.HasError()) Value = std::move(Loaded);
		}
		else if (Size != 0)
		{
			Ar.WriteBytes(std::as_bytes(std::span<const uint8>(Value)));
		}
	}

	auto SerializeBoundedString(FArchive& Ar, std::string& Value, uint64 MaximumBytes) -> void
	{
		if (Ar.HasError() || !Ar.IsCurrentFieldAvailable()) return;
		if (Ar.IsSaving() && Ar.TryCaptureLogicalText(EArchiveLogicalTextKind::String, Value)) return;
		uint64 Size = Ar.IsSaving() ? static_cast<uint64>(Value.size()) : 0;
		Ar << Size;
		if (Ar.HasError()) return;
		if (Size > MaximumBytes || Size > static_cast<uint64>(std::string().max_size()))
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded, "String exceeds its serialization limit.");
			return;
		}
		if (Ar.IsLoading())
		{
			if (Size > Ar.GetRemainingPayloadBytes())
			{
				Ar.Fail(EArchiveFailureCode::TruncatedPayload, "String payload is truncated.");
				return;
			}
			std::string Loaded(static_cast<size_t>(Size), '\0');
			if (Size != 0) Ar.ReadBytes(std::as_writable_bytes(std::span<char>(Loaded)));
			if (!Ar.HasError()) Value = std::move(Loaded);
		}
		else if (Size != 0)
		{
			Ar.WriteBytes(std::as_bytes(std::span<const char>(Value)));
		}
	}

	auto SerializeAlignment(FArchive& Ar, uint64 Alignment) -> void
	{
		if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
		{
			Ar.Fail(EArchiveFailureCode::InvalidAlignment, "Archive alignment must be a nonzero power of two.");
			return;
		}
		const uint64 Padding = (Alignment - (Ar.Tell() & (Alignment - 1))) & (Alignment - 1);
		std::array<std::byte, 64> Buffer{};
		if (Padding > Buffer.size())
		{
			Ar.Fail(EArchiveFailureCode::InvalidAlignment, "Archive alignment exceeds the supported bound.");
			return;
		}
		if (Ar.IsLoading())
		{
			Ar.ReadBytes(std::span<std::byte>(Buffer).first(static_cast<size_t>(Padding)));
			if (!Ar.HasError() && std::ranges::any_of(Buffer.begin(), Buffer.begin() + static_cast<ptrdiff_t>(Padding),
				[](std::byte Byte) { return Byte != std::byte{}; }))
				Ar.Fail(EArchiveFailureCode::NonZeroPadding, "Archive alignment padding must be zero.");
		}
		else
		{
			Ar.WriteBytes(std::span<const std::byte>(Buffer).first(static_cast<size_t>(Padding)));
		}
	}

	auto RequireArchiveEnd(FArchive& Ar) -> bool
	{
		if (Ar.HasError()) return false;
		if (Ar.GetRemainingPayloadBytes() == 0) return true;
		Ar.Fail(EArchiveFailureCode::TrailingData, "Archive contains trailing bytes.");
		return false;
	}
}
