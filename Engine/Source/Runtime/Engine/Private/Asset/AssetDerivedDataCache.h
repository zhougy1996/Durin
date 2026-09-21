#pragma once

#if DURIN_WITH_EDITOR

#include "DerivedDataCache/DerivedDataCache.h"
#include "Asset/DerivedDataCacheKeyProxy.h"
#include "Asset/AssetCacheDiagnostic.h"

namespace Durin::AssetDerivedDataCache
{
	// Family decoders may reject a byte hit and rebuild it as a miss.
	enum class ELoadResult : uint8
	{
		Hit,
		Miss
	};

	using FOperationDiagnostic = FAssetCacheDiagnostic;

	inline auto Load(
		const FCacheKeyProxy& Key,
		uint64 MaximumValueBytes,
		FSharedByteBuffer& OutBytes,
		FOperationDiagnostic& OutDiagnostic) -> ELoadResult
	{
		using namespace DerivedData;
		OutBytes = {};
		OutDiagnostic = {};
		OutDiagnostic.Key = Key;
		OutDiagnostic.MaximumValueBytes = MaximumValueBytes;
		const auto Start = std::chrono::steady_clock::now();
		FCacheGetResult Result = GetCache().Get({
			.Key = *Key.AsCacheKey(),
			.MaximumValueBytes = MaximumValueBytes});
		OutDiagnostic.DurationNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
		if (!Result)
		{
			OutDiagnostic.Code = Result.error().Code == ECacheError::Miss ? EAssetCacheError::None : EAssetCacheError::Read;
			OutDiagnostic.ReadCause = std::make_shared<FCacheError>(std::move(Result.error()));
			return ELoadResult::Miss;
		}
		OutBytes = std::move(*Result);
		return ELoadResult::Hit;
	}

	inline auto Load(const FCacheKeyProxy& Key,
		uint64 MaximumValueBytes, FByteBuffer& OutBytes,
		FOperationDiagnostic& OutDiagnostic) -> ELoadResult
	{
		OutBytes.clear();
		FSharedByteBuffer Value;
		const ELoadResult Result = Load(Key, MaximumValueBytes,
			Value, OutDiagnostic);
		if (Result == ELoadResult::Hit)
			OutBytes.assign(Value.GetBytes().begin(), Value.GetBytes().end());
		return Result;
	}

	inline auto Store(
		const FCacheKeyProxy& Key,
		FByteView Bytes,
		uint64 MaximumValueBytes,
		FOperationDiagnostic& OutDiagnostic) -> bool
	{
		using namespace DerivedData;
		OutDiagnostic = {};
		OutDiagnostic.Key = Key;
		OutDiagnostic.MaximumValueBytes = MaximumValueBytes;
		const auto Start = std::chrono::steady_clock::now();
		FCachePutResult Result = GetCache().Put({
			.Key = *Key.AsCacheKey(),
			.Value = Bytes,
			.MaximumValueBytes = MaximumValueBytes});
		OutDiagnostic.DurationNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
		if (!Result)
		{
			OutDiagnostic.Code = EAssetCacheError::Write;
			OutDiagnostic.WriteCause = std::make_shared<FCacheError>(std::move(Result.error()));
			return false;
		}
		return true;
	}
}

#endif
