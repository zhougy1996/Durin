#pragma once

#include "CoreMinimal.h"
#include "Renderers/MeshPipelineDescription.h"
#include "RendererAPI.h"
#include "RHIGPUReadback.h"
#include "RHICommandList.h"
#include "RDG/RDG.h"

namespace Durin::RendererPrivate { class FMeshDrawCommandStatsManager; }

namespace Durin
{
	struct FGPUCullingVisibilityCounts
	{
		uint32 Candidates = 0;
		uint32 Visible = 0;
		uint32 Culled = 0;
	};

	// Optional observation only. Rendering never consumes this asynchronous result.
	class RENDERER_API FGPUCullingVisibilityQuery final
	{
	public:
		FGPUCullingVisibilityQuery(uint32 Candidates,
			std::span<const FRHIDrawIndexedIndirectArguments> ExpectedArguments);
		auto GetState() const -> ERHIGPUReadbackState;
		auto GetCounts() const -> std::optional<FGPUCullingVisibilityCounts>;
	private:
		friend class RendererPrivate::FMeshDrawCommandStatsManager;
		friend struct FGPUCullingVisibilityQueryTestAccess;
		auto ResolveLocked() const -> void;
		uint32 CandidateCount;
		std::vector<FRHIDrawIndexedIndirectArguments> Expected;
		mutable FRHIGPUBufferReadback Readback;
		mutable std::mutex Mutex;
		mutable ERHIGPUReadbackState State = ERHIGPUReadbackState::Pending;
		mutable std::optional<FGPUCullingVisibilityCounts> Counts;
	};

}

namespace Durin::RendererPrivate
{
	struct FVisibleMeshDrawCommandStatsData
	{
		uint32 SourceDrawIndex = 0, NumSourceDraws = 0, NativeSubmissions = 0;
		bool bSucceeded = false, bIndirect = false;
	};
	struct FMeshDrawCommandPassStats
	{
		std::vector<FVisibleMeshDrawCommandStatsData> DrawData;
		std::shared_ptr<const FGPUCullingVisibilityQuery> GPUVisibility;
		auto Record(uint32 Source, uint32 Count, bool Success, uint32 NativeCalls, bool Indirect) -> void
		{ DrawData.push_back({Source, Count, NativeCalls, Success, Indirect}); }
	};
	template<typename TResult>
	struct TMeshDrawCommandReadbackParameters
	{
		TRDGValueRead<TResult> Completion;
		FRDGBufferParameter Arguments;
		static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
		{
			using P = TMeshDrawCommandReadbackParameters;
			static const std::array Members{
				MakeRDGValueParameterMemberMetadata<P, decltype(Completion), TResult>("Completion", offsetof(P, Completion)),
				MakeRDGResourceParameterMemberMetadata<P, decltype(Arguments), FRDGBufferParameter>("Arguments", offsetof(P, Arguments), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::TransferRead)};
			static const auto Metadata = MakeInlineRDGParametersMetadata<P>("MeshDrawCommandStats.Readback", Members);
			return &Metadata;
		}
	};
	// One render invocation owns CPU/GPU requests and pass results. Output handles
	// retain immutable observations after the command-local manager is destroyed.
	class FMeshDrawCommandStatsManager
	{
	public:
		struct FRequest { bool bDrawStats = false; bool bGPUVisibility = false; };
		FMeshDrawCommandStatsManager() = default;
		explicit FMeshDrawCommandStatsManager(FRequest InRequest) : Request(InRequest) {}
		auto CollectStats() const -> bool { return Request.bDrawStats; }
		auto CollectGPUVisibilityStats() const -> bool { return Request.bGPUVisibility; }
		auto CreatePassStats(FMeshPassKey Pass) -> FMeshDrawCommandPassStats*
		{ return CollectStats() ? &GetPassStats(Pass) : nullptr; }

		template<typename TResult, typename TReady>
		auto QueueDrawRDGIndirectArgsReadback(FRDGBuilder& Graph, FMeshPassKey Pass,
			FRDGBufferParameter Arguments, uint32 Candidates,
			std::span<const FRHIDrawIndexedIndirectArguments> Expected,
			TRDGValueHandle<TResult> Completion, TReady Ready) -> std::shared_ptr<const FGPUCullingVisibilityQuery>
		{
			if (!CollectGPUVisibilityStats()) return {};
			require(Arguments.Size && Arguments.Size <= UINT32_MAX && Arguments.Offset <= UINT32_MAX);
			auto& Statistics = GetPassStats(Pass);
			require(!Statistics.GPUVisibility);
			auto Query = std::make_shared<FGPUCullingVisibilityQuery>(Candidates, Expected);
			Statistics.GPUVisibility = Query;
			using P = TMeshDrawCommandReadbackParameters<TResult>;
			auto Parameters = Graph.AllocParameters<P>();
			Parameters->Arguments = Arguments; Parameters->Completion = {Completion};
			const auto ReadbackPass = Graph.AddPass("MeshDrawCommandStats.Readback", ERDGPassType::Copy, std::move(Parameters),
				[Query, Ready = std::move(Ready)](FRHICommandListImmediate& Commands, const P& Parameters, const FRDGParameterResolver& Resolver) {
					if (!Ready(Resolver.ReadValue(Parameters.Completion))) { Query->Readback.Cancel(); return; }
					Query->Readback.EnqueueCopy(Commands, Resolver.GetBuffer(Parameters.Arguments),
						static_cast<uint32>(Parameters.Arguments.Size), static_cast<uint32>(Parameters.Arguments.Offset));
				});
			Graph.MarkPassRoot(ReadbackPass, "Explicit mesh draw statistics observer");
			return Query;
		}
	private:
		auto GetPassStats(FMeshPassKey Pass) -> FMeshDrawCommandPassStats&
		{ if (!PassData) PassData.emplace(); return PassData->try_emplace(Pass).first->second; }
		FRequest Request;
		std::optional<std::map<FMeshPassKey, FMeshDrawCommandPassStats>> PassData;
	};
}
