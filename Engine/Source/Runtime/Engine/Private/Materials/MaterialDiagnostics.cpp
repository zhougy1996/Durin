#include "Materials/MaterialRenderTypes.h"

#include <atomic>

namespace Durin
{
	namespace
	{
		std::array<std::atomic<uint64>,
			static_cast<size_t>(EMaterialFallbackReason::Count)>
			GMaterialFallbackCounts{};
	}
	auto GetErrorMaterialRenderData() -> const FMaterialRenderData&
	{
		static const FMaterialRenderData ErrorMaterial;
		return ErrorMaterial;
	}

	auto RecordMaterialFallbackReason(EMaterialFallbackReason Reason) -> void
	{
		const size_t Index = static_cast<size_t>(Reason);
		if (Index < GMaterialFallbackCounts.size())
		{
			GMaterialFallbackCounts[Index].fetch_add(
				1, std::memory_order_relaxed);
		}
	}

	auto GetMaterialFallbackDiagnosticsSnapshot()
		-> FMaterialFallbackDiagnosticsSnapshot
	{
		FMaterialFallbackDiagnosticsSnapshot Result;
		for (size_t Index = 0; Index < GMaterialFallbackCounts.size(); ++Index)
		{
			Result.Counts[Index] = GMaterialFallbackCounts[Index].load(
				std::memory_order_relaxed);
		}
		return Result;
	}

	auto ResetMaterialFallbackDiagnosticsForTests() -> void
	{
		for (std::atomic<uint64>& Count : GMaterialFallbackCounts)
		{
			Count.store(0, std::memory_order_relaxed);
		}
	}

}
