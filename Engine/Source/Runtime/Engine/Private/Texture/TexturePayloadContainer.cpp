#include "Texture/TexturePayloadContainer.h"

#include "Hash/XxHash.h"
#include "Serialization/Archive.h"
#include "Templates/CheckedArithmetic.h"

namespace Durin::TexturePayloadContainer
{
	namespace
	{
		auto SerializeRecord(FArchive& Ar, FRecord& Record) -> void
		{
			Ar << Record.Coordinate << Record.MipIndex << Record.Width << Record.Height
				<< Record.RowPitch << Record.LayerPitch << Record.DataOffset << Record.ByteCount;
		}

		auto ValidRecordCount(const FDescriptor& Descriptor, uint32 Count) -> bool
		{
			const uint32 Slices = Descriptor.Dimension == ETexturePayloadDimension::TextureCube ? 6 : 1;
			return (Descriptor.Dimension == ETexturePayloadDimension::Texture2D
				|| Descriptor.Dimension == ETexturePayloadDimension::TextureCube
				|| Descriptor.Dimension == ETexturePayloadDimension::Texture3D)
				&& Descriptor.SliceCount == Slices && Descriptor.MipCount > 0
				&& Descriptor.MipCount <= MaximumTextureMipCount
				&& Count == Slices * Descriptor.MipCount;
		}
	}

	auto ResolveContext(FArchive& Ar, const FTexturePlatformSerializationContext& Explicit,
		FTexturePlatformSerializationContext& Context) -> bool
	{
		if (Ar.HasError()) return false;
		Context = Explicit;
		const auto& Target = Ar.GetTarget();
		if (!Target.Platform.empty() || !Target.Profile.empty())
		{
			const auto Profile = Target.Profile == "Game" ? ECookTargetProfile::Game
				: Target.Profile == "EditorValidation" ? ECookTargetProfile::EditorValidation
				: ECookTargetProfile::Invalid;
			if (Target.Platform != "Win64" || Profile == ECookTargetProfile::Invalid
				|| (Explicit.TargetPlatform != ECookTargetPlatform::Invalid && Explicit.TargetPlatform != ECookTargetPlatform::Win64)
				|| (Explicit.TargetProfile != ECookTargetProfile::Invalid && Explicit.TargetProfile != Profile))
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "Texture Archive target is missing, unsupported or conflicting.");
				return false;
			}
			Context = {ECookTargetPlatform::Win64, Profile};
		}
		if (Context.TargetPlatform != ECookTargetPlatform::Win64
			|| (Context.TargetProfile != ECookTargetProfile::Game && Context.TargetProfile != ECookTargetProfile::EditorValidation))
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "Texture payload requires a concrete target context.");
			return false;
		}
		return true;
	}

	auto Serialize(FArchive& Ar, FDescriptor& Descriptor,
		std::vector<FPayloadRecord>& Records,
		ECookTargetPlatform ExpectedPlatform, ECookTargetProfile ExpectedProfile) -> void
	{
		if (Ar.HasError()) return;
		const auto& Target = Ar.GetTarget();
		const std::string_view ProfileName = ExpectedProfile == ECookTargetProfile::Game
			? "Game" : "EditorValidation";
		if (ExpectedPlatform != ECookTargetPlatform::Win64
			|| (ExpectedProfile != ECookTargetProfile::Game
				&& ExpectedProfile != ECookTargetProfile::EditorValidation)
			|| (!Target.Platform.empty() && Target.Platform != "Win64")
			|| (!Target.Profile.empty() && Target.Profile != ProfileName))
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "Texture payload target context is unsupported or conflicting.");
			return;
		}

		uint32 Reserved0 = 0, Schema = TexturePayloadSchemaVersion;
		uint32 HeaderSize = TexturePayloadHeaderSize, RecordSize = TexturePayloadRecordSize;
		uint32 RecordCount = Ar.IsSaving() ? static_cast<uint32>(Records.size()) : 0;
		uint64 TableOffset = TexturePayloadHeaderSize, StoredSize = 0, StoredHash = 0, Reserved = 0;
		FByteBuffer Body;
		if (Ar.IsSaving())
		{
			if (Records.size() > MaximumTextureMipCount * 6 || !ValidRecordCount(Descriptor, RecordCount))
			{
				Ar.Fail(EArchiveFailureCode::LimitExceeded, "Texture payload record count is invalid.");
				return;
			}
			uint64 Offset = TexturePayloadHeaderSize + static_cast<uint64>(RecordCount) * TexturePayloadRecordSize;
			for (FPayloadRecord& Record : Records)
			{
				if (!TryAlignUp(Offset, TexturePayloadAlignment, MaximumTexturePayloadBytes, Offset)
					|| Record.Data.size() > MaximumTexturePayloadBytes - Offset)
				{
					Ar.Fail(EArchiveFailureCode::LimitExceeded, "Texture payload exceeds its byte limit.");
					return;
				}
				Record.Record.DataOffset = Offset;
				Record.Record.ByteCount = Record.Data.size();
				Offset += Record.Data.size();
			}
			StoredSize = Offset;
			// The complete layout is bounded before any body allocation. Staging is
			// required because the historical header hashes the following body.
			Body.reserve(static_cast<size_t>(StoredSize - TexturePayloadHeaderSize));
			FCanonicalMemoryWriter BodyAr(Body);
			for (FPayloadRecord& Record : Records) SerializeRecord(BodyAr, Record.Record);
			for (const FPayloadRecord& Record : Records)
			{
				while (BodyAr.Tell() + TexturePayloadHeaderSize < Record.Record.DataOffset)
				{
					uint8 Padding = 0;
					BodyAr << Padding;
				}
				BodyAr.WriteBytes(Record.Data);
			}
			if (BodyAr.HasError())
			{
				Ar.Fail(BodyAr.GetFailure()->Code, BodyAr.GetFailure()->Message);
				return;
			}
			StoredHash = FXxHash64::HashBuffer(Body).HashValue;
		}

		Ar << Reserved0 << Schema << Descriptor.ProducerVersion << Descriptor.TargetPlatform
			<< Descriptor.TargetProfile << Descriptor.Dimension << Descriptor.StableFormat
			<< Descriptor.SliceCount << Descriptor.MipCount << HeaderSize << RecordCount
			<< RecordSize << TableOffset << StoredSize << StoredHash << Reserved;
		if (Ar.HasError()) return;
		if (Schema != TexturePayloadSchemaVersion)
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedVersion, "Texture payload schema version is unsupported.");
			return;
		}
		if (Descriptor.TargetPlatform != ExpectedPlatform || Descriptor.TargetProfile != ExpectedProfile)
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "Texture payload target platform or profile does not match.");
			return;
		}
		if (Reserved0 != 0 || Reserved != 0 || HeaderSize != TexturePayloadHeaderSize
			|| RecordSize != TexturePayloadRecordSize || TableOffset != TexturePayloadHeaderSize)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Texture payload container layout is invalid.");
			return;
		}
		if (!ValidRecordCount(Descriptor, RecordCount) || StoredSize > MaximumTexturePayloadBytes)
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded, "Texture payload count or stored size exceeds its limit.");
			return;
		}
		const uint64 TableBytes = static_cast<uint64>(RecordCount) * RecordSize;
		if (StoredSize < HeaderSize || TableBytes > StoredSize - HeaderSize)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Texture payload record table is outside its extent.");
			return;
		}
		if (Ar.IsSaving())
		{
			Ar.WriteBytes(Body);
			return;
		}

		FByteView Region;
		if (!Ar.ReadRegion(StoredSize - HeaderSize, Region)) return;
		if (FXxHash64::HashBuffer(Region).HashValue != StoredHash)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Texture payload checksum does not match.");
			return;
		}
		FCanonicalMemoryReader BodyAr(Region);
		Records.clear();
		Records.resize(RecordCount);
		uint64 PreviousEnd = HeaderSize + TableBytes;
		for (FPayloadRecord& Entry : Records)
		{
			FRecord& Record = Entry.Record;
			SerializeRecord(BodyAr, Record);
			if (BodyAr.HasError())
			{
				Ar.Fail(BodyAr.GetFailure()->Code, BodyAr.GetFailure()->Message);
				return;
			}
			if (Record.DataOffset % TexturePayloadAlignment != 0 || Record.DataOffset < PreviousEnd
				|| Record.DataOffset > StoredSize || Record.ByteCount > StoredSize - Record.DataOffset)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, "Texture payload record range is invalid.");
				return;
			}
			for (uint64 Offset = PreviousEnd; Offset < Record.DataOffset; ++Offset)
				if (Region[static_cast<size_t>(Offset - HeaderSize)] != std::byte{0})
				{
					Ar.Fail(EArchiveFailureCode::NonZeroPadding, "Texture payload alignment padding is nonzero.");
					return;
				}
			Entry.Data = Region.subspan(static_cast<size_t>(Record.DataOffset - HeaderSize),
				static_cast<size_t>(Record.ByteCount));
			PreviousEnd = Record.DataOffset + Record.ByteCount;
		}
		if (PreviousEnd != StoredSize)
			Ar.Fail(EArchiveFailureCode::TrailingData, "Texture payload contains trailing data.");
	}

}
