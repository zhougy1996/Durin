#pragma once

#if DURIN_WITH_EDITOR

#include "Asset/AssetDerivedDataCache.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"

namespace Durin::TextureDerivedDataCache
{
	using AssetDerivedDataCache::ELoadResult;
	using AssetDerivedDataCache::FOperationDiagnostic;

	template <typename PlatformDataType>
	auto Load(const FCacheKeyProxy& Key,
		ECookTargetPlatform TargetPlatform, ECookTargetProfile TargetProfile,
		PlatformDataType& OutPlatformData,
		FOperationDiagnostic& OutDiagnostic) -> ELoadResult
	{
		FSharedByteBuffer Bytes;
		if (AssetDerivedDataCache::Load(Key,
			MaximumTexturePayloadBytes, Bytes, OutDiagnostic) == ELoadResult::Miss)
			return ELoadResult::Miss;

		// The provider owns this unpublished destination and discards it on a miss.
		FCanonicalMemoryReader Ar(
			Bytes.GetBytes(), EArchivePurpose::DerivedDataPayload,
			{.Target = {TargetPlatform == ECookTargetPlatform::Win64 ? "Win64" : "",
				TargetProfile == ECookTargetProfile::Game ? "Game"
				: TargetProfile == ECookTargetProfile::EditorValidation ? "EditorValidation" : ""}});
		OutPlatformData.Serialize(Ar);
		if (Ar.HasError() || !RequireArchiveEnd(Ar) || !OutPlatformData.IsValid())
		{
			OutDiagnostic.Message = AssetDerivedDataCache::BoundDiagnostic(
				Ar.GetFailure() ? Ar.GetFailure()->Message
					: "Texture DDC payload is invalid or has trailing bytes.");
			return ELoadResult::Miss;
		}
		OutDiagnostic.Message.clear();
		return ELoadResult::Hit;
	}

	template <typename PlatformDataType>
	auto Store(const FCacheKeyProxy& Key,
		ECookTargetPlatform TargetPlatform, ECookTargetProfile TargetProfile,
		PlatformDataType& PlatformData,
		FOperationDiagnostic& OutDiagnostic) -> bool
	{
		OutDiagnostic = {};
		FByteBuffer Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload,
			{.Target = {TargetPlatform == ECookTargetPlatform::Win64 ? "Win64" : "",
				TargetProfile == ECookTargetProfile::Game ? "Game"
				: TargetProfile == ECookTargetProfile::EditorValidation ? "EditorValidation" : ""}});
		PlatformData.Serialize(Ar);
		if (Ar.HasError())
		{
			OutDiagnostic.Message = AssetDerivedDataCache::BoundDiagnostic(
				Ar.GetFailure() ? Ar.GetFailure()->Message
					: "Texture DDC payload serialization failed.");
			return false;
		}

		return AssetDerivedDataCache::Store(Key, Bytes,
			MaximumTexturePayloadBytes, OutDiagnostic);
	}
}

#endif
