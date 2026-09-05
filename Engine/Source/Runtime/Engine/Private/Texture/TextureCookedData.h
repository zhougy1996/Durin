#pragma once

#include "Asset/BulkData.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"

#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Durin::TexturePrivate
{
	// The caller supplies the historical wire owner; storage ownership must not
	// change the native field identity or the family-specific payload codec.
	template<class TPlatformData>
	auto SerializeCookedPlatformData(FArchive& Ar, FBulkData& CookedData,
		TPlatformData* PlatformData, FName WireOwner, std::string_view Family) -> void
	{
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				std::format("{} cooked platform data requires the Win64 Game target.", Family));
			return;
		}
		FBulkData Projection;
		FBulkData* Value = &CookedData;
		if (Ar.IsSaving())
		{
			if (!PlatformData || !PlatformData->IsValid())
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					std::format("{} cooked platform data is unavailable.", Family));
				return;
			}
			FByteBuffer Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload);
			PlatformData->Serialize(Writer, {
				.TargetPlatform = ECookTargetPlatform::Win64,
				.TargetProfile = ECookTargetProfile::Game});
			std::string Error;
			if (Writer.HasError() || !FBulkData::TryCreateDetached(Bytes, Projection, &Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error.empty()
					? std::string(Writer.GetError()) : std::move(Error));
				return;
			}
			Value = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {WireOwner,
			FName("PlatformData"), FArchiveLogicalTypeDescriptor::BulkData()});
		Value->Serialize(Ar, {.Alignment = TexturePayloadAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	// Decode into detached typed data and release the bulk lock before publishing
	// through the family's validating setter. A failure never updates resources.
	template<class TPlatformData, class TTexture>
	auto LoadCookedPlatformData(TTexture& Texture, FBulkData& CookedData,
		std::string_view Family, std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			OutError = std::format("Cooked {} '{}': {}",
				Family, Texture.GetObjectPath(), Message);
			return false;
		};
		FByteView Bytes;
		if (!CookedData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);
		auto Candidate = std::make_unique<TPlatformData>();
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::CookedPayload);
		Candidate->Serialize(Ar, {.TargetPlatform = ECookTargetPlatform::Win64,
			.TargetProfile = ECookTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar))
		{
			CookedData.UnlockReadOnly();
			return FailCooked(std::string(Ar.GetError()));
		}
		if (!CookedData.UnlockReadOnly(&OutError)) return FailCooked(OutError);
		if (!Texture.SetPlatformData(std::move(Candidate), OutError))
			return FailCooked(OutError);
		Texture.UpdateResource();
		OutError.clear();
		return true;
	}
}
