#include "Renderers/MeshDrawCommandStats.h"

namespace Durin
{
	FGPUCullingVisibilityQuery::FGPUCullingVisibilityQuery(uint32 Candidates,
		std::span<const FRHIDrawIndexedIndirectArguments> ExpectedArguments)
		: CandidateCount(Candidates), Expected(ExpectedArguments.begin(), ExpectedArguments.end())
	{
	}

	auto FGPUCullingVisibilityQuery::ResolveLocked() const -> void
	{
		if (State != ERHIGPUReadbackState::Pending) return;
		State = Readback.GetState();
		if (State != ERHIGPUReadbackState::Ready) return;
		const uint64 NumBytes = Expected.size() * sizeof(FRHIDrawIndexedIndirectArguments);
		if (Expected.empty() || !CandidateCount || NumBytes > UINT32_MAX || Readback.GetDataSize() != NumBytes) { State = ERHIGPUReadbackState::Failed; return; }
		const auto* Bytes = static_cast<const std::byte*>(Readback.Lock(static_cast<uint32>(NumBytes)));
		if (!Bytes) { State = ERHIGPUReadbackState::Failed; return; }
		auto Parse = [&]() -> std::optional<FGPUCullingVisibilityCounts> {
			if (Expected.front().FirstInstance != 0) return {};
			uint32 Visible = 0;
			for (size_t Index = 0; Index < Expected.size(); ++Index)
			{
				FRHIDrawIndexedIndirectArguments Actual;
				std::memcpy(&Actual, Bytes + Index * sizeof(Actual), sizeof(Actual));
				const auto& Reference = Expected[Index];
				const uint32 End = Index + 1 < Expected.size() ? Expected[Index + 1].FirstInstance : CandidateCount;
				if (End <= Reference.FirstInstance || End > CandidateCount || Actual.IndexCount != Reference.IndexCount
					|| Actual.FirstIndex != Reference.FirstIndex || Actual.VertexOffset != Reference.VertexOffset
					|| Actual.FirstInstance != Reference.FirstInstance || Actual.InstanceCount > End - Reference.FirstInstance) return {};
				Visible += Actual.InstanceCount;
			}
			return FGPUCullingVisibilityCounts{CandidateCount, Visible, CandidateCount - Visible};
		};
		Counts = Parse(); Readback.Unlock();
		if (!Counts) State = ERHIGPUReadbackState::Failed;
	}

	auto FGPUCullingVisibilityQuery::GetState() const -> ERHIGPUReadbackState
	{
		std::lock_guard Lock(Mutex);
		ResolveLocked();
		return State;
	}

	auto FGPUCullingVisibilityQuery::GetCounts() const -> std::optional<FGPUCullingVisibilityCounts>
	{
		std::lock_guard Lock(Mutex);
		ResolveLocked();
		return Counts;
	}

}
