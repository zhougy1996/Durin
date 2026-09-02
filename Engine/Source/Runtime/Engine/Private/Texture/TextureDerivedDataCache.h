#pragma once

#if DURIN_WITH_EDITOR

#include "DerivedDataCache/DerivedDataCache.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"

namespace Durin::TextureDerivedDataCache
{
	inline constexpr std::string_view Texture2DBucket = "Textures/Objects";
	inline constexpr std::string_view TextureCubeBucket = "TextureCube/Objects";
	inline constexpr std::string_view VolumeTextureBucket = "VolumeTexture/Objects";

	enum class ELoadResult : uint8
	{
		Hit,
		Miss
	};

	struct FOperationDiagnostic
	{
		uint64 DurationNanoseconds = 0;
		std::string Message;
	};

	template <typename PlatformDataType>
	auto Load(std::string_view BucketName, std::string_view Key,
		ECookTargetPlatform TargetPlatform, ECookTargetProfile TargetProfile,
		PlatformDataType& OutPlatformData,
		FOperationDiagnostic& OutDiagnostic) -> ELoadResult
	{
		using namespace DerivedData;
		OutDiagnostic = {};
		const FCacheBucket Bucket = FCacheBucket::FromString(BucketName);
		const FCacheKey CacheKey = FCacheKey::FromString(Key);
		const auto Start = std::chrono::steady_clock::now();
		const FCacheGetResult Result = DerivedData::GetCache().Get({
			.Bucket = Bucket,
			.Key = CacheKey,
			.MaximumValueBytes = MaximumTexturePayloadBytes});
		OutDiagnostic.DurationNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
		if (Result.Status != ECacheGetStatus::Hit)
		{
			OutDiagnostic.Message = Result.Diagnostic;
			return ELoadResult::Miss;
		}

		PlatformDataType Candidate;
		FCanonicalMemoryReader Ar(
			Result.Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
		Candidate.Serialize(Ar, {
			.TargetPlatform = TargetPlatform,
			.TargetProfile = TargetProfile});
		if (Ar.HasError() || !RequireArchiveEnd(Ar) || !Candidate.IsValid())
		{
			OutDiagnostic.Message = Ar.GetFailure() ? Ar.GetFailure()->Message
				: "Texture DDC payload is invalid or has trailing bytes.";
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
		using namespace DerivedData;
		OutDiagnostic = {};
		const auto Start = std::chrono::steady_clock::now();
		const auto Finish = [&] {
			OutDiagnostic.DurationNanoseconds = static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - Start).count());
		};
		FByteArray Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		PlatformData.Serialize(Ar, {
			.TargetPlatform = TargetPlatform,
			.TargetProfile = TargetProfile});
		if (Ar.HasError())
		{
			OutDiagnostic.Message = Ar.GetFailure() ? Ar.GetFailure()->Message
				: "Texture DDC payload serialization failed.";
			Finish();
			return false;
		}

		const FCacheBucket Bucket = FCacheBucket::FromString(BucketName);
		const FCacheKey CacheKey = FCacheKey::FromString(Key);
		const FCachePutResult Put = DerivedData::GetCache().Put({
			.Bucket = Bucket,
			.Key = CacheKey,
			.Value = Bytes,
			.MaximumValueBytes = MaximumTexturePayloadBytes});
		if (!Put)
		{
			OutDiagnostic.Message = Put.Diagnostic;
			Finish();
			return false;
		}
		OutDiagnostic.Message.clear();
		Finish();
		return true;
	}
}

#endif
