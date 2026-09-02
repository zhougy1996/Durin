#pragma once

#if DURIN_WITH_EDITOR

#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::AssetDerivedDataCache
{
	inline constexpr size_t MaximumDiagnosticBytes = 2048;

	inline auto BoundDiagnostic(std::string Message) -> std::string
	{
		if (Message.size() > MaximumDiagnosticBytes)
			Message.resize(MaximumDiagnosticBytes);
		return Message;
	}

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

	inline auto Load(
		std::string_view BucketName,
		std::string_view Key,
		uint64 MaximumValueBytes,
		FByteArray& OutBytes,
		FOperationDiagnostic& OutDiagnostic) -> ELoadResult
	{
		using namespace DerivedData;
		OutBytes.clear();
		OutDiagnostic = {};
		const auto Start = std::chrono::steady_clock::now();
		const FCacheGetResult Result = GetCache().Get({
			.Bucket = FCacheBucket::FromString(BucketName),
			.Key = FCacheKey::FromString(Key),
			.MaximumValueBytes = MaximumValueBytes});
		OutDiagnostic.DurationNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
		if (Result.Status != ECacheGetStatus::Hit)
		{
			OutDiagnostic.Message = BoundDiagnostic(Result.Diagnostic);
			return ELoadResult::Miss;
		}
		OutBytes.assign(Result.Value.GetBytes().begin(), Result.Value.GetBytes().end());
		return ELoadResult::Hit;
	}

	inline auto Store(
		std::string_view BucketName,
		std::string_view Key,
		std::span<const std::byte> Bytes,
		uint64 MaximumValueBytes,
		FOperationDiagnostic& OutDiagnostic) -> bool
	{
		using namespace DerivedData;
		OutDiagnostic = {};
		const auto Start = std::chrono::steady_clock::now();
		const FCachePutResult Result = GetCache().Put({
			.Bucket = FCacheBucket::FromString(BucketName),
			.Key = FCacheKey::FromString(Key),
			.Value = Bytes,
			.MaximumValueBytes = MaximumValueBytes});
		OutDiagnostic.DurationNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
		if (!Result)
		{
			OutDiagnostic.Message = BoundDiagnostic(Result.Diagnostic);
			return false;
		}
		return true;
	}
}

#endif
