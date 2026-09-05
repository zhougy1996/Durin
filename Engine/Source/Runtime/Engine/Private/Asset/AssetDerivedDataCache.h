#pragma once

#if DURIN_WITH_EDITOR

#include "DerivedDataCache/DerivedDataCache.h"
#include "DerivedDataCacheKeyProxy.h"

namespace Durin::AssetDerivedDataCache
{
	inline constexpr size_t MaximumDiagnosticBytes = 2048;

	inline auto BoundDiagnostic(std::string Message) -> std::string
	{
		if (Message.size() > MaximumDiagnosticBytes)
			Message.resize(MaximumDiagnosticBytes);
		return Message;
	}

	// Family decoders may reject a byte hit and rebuild it as a miss.
	enum class ELoadResult : uint8
	{
		Hit,
		Miss
	};

	// Measures only the cache Get/Put call, excluding payload codecs and copies.
	struct FOperationDiagnostic
	{
		uint64 DurationNanoseconds = 0;
		std::string Message;
	};

	// Preserve both recovery and persistence failures within one bounded result.
	inline auto CombineDiagnostics(const FOperationDiagnostic& Read,
		const FOperationDiagnostic& Write) -> std::string
	{
		if (Read.Message.empty()) return BoundDiagnostic(Write.Message);
		if (Write.Message.empty()) return BoundDiagnostic(Read.Message);
		constexpr size_t MessageBudget = (MaximumDiagnosticBytes - 13) / 2;
		return "Read: " + Read.Message.substr(0, MessageBudget)
			+ "; Put: " + Write.Message.substr(0, MessageBudget);
	}

	inline auto Load(
		const FCacheKeyProxy& Key,
		uint64 MaximumValueBytes,
		FSharedByteBuffer& OutBytes,
		FOperationDiagnostic& OutDiagnostic) -> ELoadResult
	{
		using namespace DerivedData;
		OutBytes = {};
		OutDiagnostic = {};
		const auto Start = std::chrono::steady_clock::now();
		FCacheGetResult Result = GetCache().Get({
			.Key = *Key.AsCacheKey(),
			.MaximumValueBytes = MaximumValueBytes});
		OutDiagnostic.DurationNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
		if (Result.Status != ECacheGetStatus::Hit)
		{
			OutDiagnostic.Message = BoundDiagnostic(Result.Diagnostic);
			return ELoadResult::Miss;
		}
		OutBytes = std::move(Result.Value);
		return ELoadResult::Hit;
	}

	inline auto Load(const FCacheKeyProxy& Key,
		uint64 MaximumValueBytes, FByteArray& OutBytes,
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
		std::span<const std::byte> Bytes,
		uint64 MaximumValueBytes,
		FOperationDiagnostic& OutDiagnostic) -> bool
	{
		using namespace DerivedData;
		OutDiagnostic = {};
		const auto Start = std::chrono::steady_clock::now();
		const FCachePutResult Result = GetCache().Put({
			.Key = *Key.AsCacheKey(),
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
