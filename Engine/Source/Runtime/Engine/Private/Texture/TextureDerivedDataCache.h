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
		if (Ar.IsError() || !RequireArchiveEnd(Ar) || !OutPlatformData.IsValid())
		{
			OutDiagnostic.Code = EAssetCacheError::Decode;
			if (Ar.GetFailure()) OutDiagnostic.ArchiveCause = *Ar.GetFailure();
			return ELoadResult::Miss;
		}
		return ELoadResult::Hit;
	}

	template <typename PlatformDataType>
	auto Store(const FCacheKeyProxy& Key,
		ECookTargetPlatform TargetPlatform, ECookTargetProfile TargetProfile,
		PlatformDataType& PlatformData,
		FOperationDiagnostic& OutDiagnostic) -> bool
	{
		OutDiagnostic = {};
		OutDiagnostic.Key = Key;
		OutDiagnostic.MaximumValueBytes = MaximumTexturePayloadBytes;
		FByteBuffer Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload,
			{.Target = {TargetPlatform == ECookTargetPlatform::Win64 ? "Win64" : "",
				TargetProfile == ECookTargetProfile::Game ? "Game"
				: TargetProfile == ECookTargetProfile::EditorValidation ? "EditorValidation" : ""}});
		PlatformData.Serialize(Ar);
		if (Ar.IsError())
		{
			OutDiagnostic.Code = EAssetCacheError::Encode;
			if (Ar.GetFailure()) OutDiagnostic.ArchiveCause = *Ar.GetFailure();
			return false;
		}

		return AssetDerivedDataCache::Store(Key, Bytes,
			MaximumTexturePayloadBytes, OutDiagnostic);
	}
}

#endif
