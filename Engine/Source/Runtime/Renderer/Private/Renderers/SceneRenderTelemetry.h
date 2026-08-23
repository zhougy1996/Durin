#pragma once

#include "Renderers/ViewRenderCounters.h"

namespace Durin
{
	struct FPreparedStaticMeshView;
	struct FResolvedStaticMeshView;
	struct FPreparedSkeletalMeshView;
	struct FResolvedSkeletalMeshView;
	struct FResolvedSkeletalPaletteTable;
	struct FPreparedTerrainView;
	struct FResolvedTerrainView;

	// Owns the command-local diagnostic accumulator. Feature preparation and
	// resolved execution values remain the authoritative sources; this value is
	// only reduced/published after a successful frame transaction.
	struct FSceneRenderTelemetry
	{
		FViewRenderCounters Counters;
	};

	class RENDERER_API FSceneTelemetryPublication final
	{
	public:
		FSceneTelemetryPublication(
			const FSceneRenderTelemetry& InTelemetry,
			FSceneViewStatistics* InOutStatistics)
			: Telemetry(InTelemetry), OutStatistics(InOutStatistics)
		{
		}

		auto Commit() -> void;

	private:
		const FSceneRenderTelemetry& Telemetry;
		FSceneViewStatistics* OutStatistics = nullptr;
		bool bCommitted = false;
	};

	auto ReduceStaticMeshTelemetry(
		const FPreparedStaticMeshView& Prepared,
		const FResolvedStaticMeshView& Resolved,
		FViewRenderCounters& Counters) -> void;
	auto ReduceSkeletalMeshTelemetry(
		const FPreparedSkeletalMeshView& Prepared,
		const FResolvedSkeletalMeshView& Resolved,
		const FResolvedSkeletalPaletteTable& Palettes,
		FViewRenderCounters& Counters) -> void;
	auto ReduceTerrainTelemetry(
		const FPreparedTerrainView& Prepared,
		const FResolvedTerrainView& Resolved,
		FViewRenderCounters& Counters) -> void;
} // namespace Durin
