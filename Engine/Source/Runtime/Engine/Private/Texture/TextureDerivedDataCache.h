#pragma once

#if DURIN_WITH_EDITOR

#include "Asset/AssetDerivedDataCache.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"

namespace Durin::TextureDerivedDataCache
{
	inline constexpr std::string_view Texture2DBucket = "Textures/Objects";
	inline constexpr std::string_view TextureCubeBucket = "TextureCube/Objects";
	inline constexpr std::string_view VolumeTextureBucket = "VolumeTexture/Objects";

	using AssetDerivedDataCache::ELoadResult;
	using AssetDerivedDataCache::FOperationDiagnostic;

	template <typename PlatformDataType>
	auto Load(std::string_view BucketName, std::string_view Key,
		ECookTargetPlatform TargetPlatform, ECookTargetProfile TargetProfile,
		PlatformDataType& OutPlatformData,
		FOperationDiagnostic& OutDiagnostic) -> ELoadResult
	{
		FSharedByteBuffer Bytes;
		if (AssetDerivedDataCache::Load(BucketName, Key,
			MaximumTexturePayloadBytes, Bytes, OutDiagnostic) == ELoadResult::Miss)
			return ELoadResult::Miss;

		PlatformDataType Candidate;
		FCanonicalMemoryReader Ar(
			Bytes.GetBytes(), EArchivePurpose::DerivedDataPayload);
		Candidate.Serialize(Ar, {
			.TargetPlatform = TargetPlatform,
			.TargetProfile = TargetProfile});
		if (Ar.HasError() || !RequireArchiveEnd(Ar) || !Candidate.IsValid())
		{
			OutDiagnostic.Message = AssetDerivedDataCache::BoundDiagnostic(
				Ar.GetFailure() ? Ar.GetFailure()->Message
					: "Texture DDC payload is invalid or has trailing bytes.");
			return ELoadResult::Miss;
		}
		OutPlatformData = std::move(Candidate);
		OutDiagnostic.Message.clear();
		return ELoadResult::Hit;
	}

	template <typename PlatformDataType>
	auto Store(std::string_view BucketName, std::string_view Key,
		ECookTargetPlatform TargetPlatform, ECookTargetProfile TargetProfile,
		PlatformDataType& PlatformData,
		FOperationDiagnostic& OutDiagnostic) -> bool
	{
		OutDiagnostic = {};
		FByteArray Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		PlatformData.Serialize(Ar, {
			.TargetPlatform = TargetPlatform,
			.TargetProfile = TargetProfile});
		if (Ar.HasError())
		{
			OutDiagnostic.Message = AssetDerivedDataCache::BoundDiagnostic(
				Ar.GetFailure() ? Ar.GetFailure()->Message
					: "Texture DDC payload serialization failed.");
			return false;
		}

		return AssetDerivedDataCache::Store(BucketName, Key, Bytes,
			MaximumTexturePayloadBytes, OutDiagnostic);
	}
}

#endif
