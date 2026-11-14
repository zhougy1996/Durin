#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshVertexFactory.h"
#include "RHICommandList.h"
#include "InstanceUniformShaderParameters.h"

namespace Durin::RendererPrivate
{
	namespace
	{
		auto Combine(size_t& Hash, uint64 Value) -> void
		{ Hash ^= std::hash<uint64>{}(Value) + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2); }
		auto BindingHash(const FMeshDrawShaderBindings& Bindings) -> size_t
		{
			size_t Hash = reinterpret_cast<uintptr_t>(Bindings.Shader);
			for (const auto& Binding : Bindings.Parameters)
			{
				Combine(Hash, reinterpret_cast<uintptr_t>(Binding.Resource));
				for (uint64 Value : {uint64(Binding.SetIndex), uint64(Binding.BindingIndex), uint64(Binding.ArrayElement),
					uint64(Binding.Type), uint64(Binding.Offset), uint64(Binding.Size), uint64(Binding.ResourceIndex)}) Combine(Hash, Value);
			}
			return Hash;
		}
		auto GeometryWithoutInstanceOffset(const FMeshDrawCommand& Command)
		{
			auto Args = Command.DrawParameters;
			std::visit([](auto& Draw) { Draw.FirstInstance = 0; }, Args);
			return Args;
		}
	}

	auto FMeshDrawCommand::MatchesForDynamicInstancing(const FMeshDrawCommand& Other) const -> bool
	{
		if (!bFinalized || !Other.bFinalized || !Pipeline || !Other.Pipeline || IndirectArgs || Other.IndirectArgs
			|| !Pipeline->VertexDeclaration || !Other.Pipeline->VertexDeclaration) return false;
		const auto A = Pipeline->GetIdentity(), B = Other.Pipeline->GetIdentity();
		return (A == B || *A == *B)
			&& VertexStreams == Other.VertexStreams && IndexBuffer == Other.IndexBuffer && IndexBufferOffset == Other.IndexBufferOffset
			&& GeometryWithoutInstanceOffset(*this) == GeometryWithoutInstanceOffset(Other)
			&& VertexBindings == Other.VertexBindings && FragmentBindings == Other.FragmentBindings && DepthBias == Other.DepthBias;
	}

	auto FMeshDrawCommand::GetDynamicInstancingHash() const -> uint32
	{
		// A hash only narrows candidates. Full effective state, including PSO values,
		// layout and bindings, is compared before a command enters the same bucket.
		size_t Hash = BindingHash(VertexBindings); Combine(Hash, BindingHash(FragmentBindings));
		Combine(Hash, reinterpret_cast<uintptr_t>(IndexBuffer)); Combine(Hash, IndexBufferOffset);
		for (const auto& Stream : VertexStreams)
		{
			Combine(Hash, Stream.StreamIndex); Combine(Hash, reinterpret_cast<uintptr_t>(Stream.VertexBuffer));
			Combine(Hash, Stream.Offset); Combine(Hash, Stream.Stride);
		}
		Combine(Hash, DrawParameters.index());
		std::visit([&](const auto& Draw) {
			Combine(Hash, Draw.InstanceCount);
			if constexpr (std::same_as<std::decay_t<decltype(Draw)>, FRHIDrawArguments>)
			{ Combine(Hash, Draw.VertexCount); Combine(Hash, Draw.FirstVertex); }
			else { Combine(Hash, Draw.IndexCount); Combine(Hash, Draw.FirstIndex); Combine(Hash, uint32(Draw.VertexOffset)); }
		}, DrawParameters);
		for (float Bias : DepthBias) Combine(Hash, std::hash<float>{}(Bias));
		return uint32(Hash) ^ uint32(Hash >> 32);
	}

	auto FDynamicMeshDrawCommandStorage::PublishVisible(FVisibleMeshDrawCommand Visible,
		std::shared_ptr<const FMeshPassBindings> Pass) -> FMeshDrawCommandHandle
	{
		require(Pass && Visible.Command && Visible.Command->Pipeline);
		const auto Key = Visible.Command->Pipeline->GetPassKey();
		auto& List = Data->Visible[Key];
		Visible.Pass = Pass.get(); List.push_back(Visible);
		Data->RetainedPasses.push_back(std::move(Pass));
		return {Key, List.size() - 1, Data->Id};
	}

	auto CollectMeshDrawCommands(const FPreparedStaticMeshView& Prepared,
		const FResolvedStaticMeshView& Resolved, FMeshPassKey Pass, bool bAllowDynamicInstancing) -> FMeshCommandOneFrameArray
	{
		FMeshCommandOneFrameArray Result(Resolved.Draws.size());
		for (uint32 Index = 0; Index < Prepared.GetNumSections(); ++Index)
		{
			const auto& Draw = Prepared.GetDraw(Index);
			const auto* Primitive = Prepared.GetPrimitive(Draw);
			if (!Primitive || !Resolved.IsReady(Draw) || Draw.ResolvedIndex >= Result.size()) continue;
			const auto Command = Resolved.GetMeshDrawCommand(Draw, Pass);
			if (!Command) continue;
			auto& Visible = Result[Draw.ResolvedIndex];
			if (const auto* Existing = Resolved.GetVisibleMeshDrawCommand(Draw, Pass, bAllowDynamicInstancing)) Visible = *Existing;
			else
			{
				auto Bindings = std::make_shared<FMeshPassBindings>(FMeshPassBindings{Command->VertexPassBindings, Command->FragmentPassBindings});
				const auto Handle = Resolved.DrawCommandStorage->PublishVisible({.Command = Command.get(),
					.DepthBias = Command->DepthBias, .PrimitiveIdBufferOffset = Command->InstanceIdOffset}, Bindings);
				Visible = *Resolved.DrawCommandStorage->GetVisible(Handle);
			}
			if (Draw.PrimitiveIndex < Resolved.GPUScenePrimitives.size()) Visible.SceneData = Resolved.GPUScenePrimitives[Draw.PrimitiveIndex];
			Visible.bSupportsDynamicInstancing = Index < Prepared.Opaque.size() + Prepared.Masked.size()
				&& Primitive->VertexDomain == EVertexDeformationDomain::Local && Draw.Command && Draw.Command->Geometry.InstanceCount == 1
				&& Primitive->CollectedBinding && std::ranges::none_of(Primitive->CollectedBinding->DeclarationElements,
					[](const auto& Element) { return Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance; });
			Visible.bSupportsInstanceCulling = Index < Prepared.Opaque.size() + Prepared.Masked.size() && Primitive->VertexDomain == EVertexDeformationDomain::Local
				&& Draw.Command && Draw.Command->Geometry.bIndexed && Draw.Command->Geometry.InstanceCount == 1
				&& (Pass.Pass != EMeshPipelinePass::GBuffer || Draw.Command->bSupportsGBuffer)
				&& Primitive->WorldBounds.bIsValid && Math::IsFinite(Primitive->WorldBounds.Min) && Math::IsFinite(Primitive->WorldBounds.Max)
				&& Primitive->CollectedBinding && std::ranges::none_of(Primitive->CollectedBinding->DeclarationElements,
					[](const auto& Element) { return Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance; });
		}
		return Result;
	}

	auto FVisibleMeshDrawCommand::MatchesForDynamicInstancing(const FVisibleMeshDrawCommand& Other) const -> bool
	{
		if (!Command || !Other.Command || !Pass || !Other.Pass || !SceneData.Buffers || !Other.SceneData.Buffers
			|| (CachedInfo && CachedInfo->CommandIndex == INDEX_NONE)
			|| (Other.CachedInfo && Other.CachedInfo->CommandIndex == INDEX_NONE)) return false;
		// Invalidated cached commands retain their value memory, not the borrowed
		// pipeline/resources. Reject their source metadata before dereferencing it.
		if (!Command->bFinalized || !Other.Command->bFinalized || !Command->Pipeline || !Other.Command->Pipeline
			|| !Command->Pipeline->VertexDeclaration || !Other.Command->Pipeline->VertexDeclaration
			|| Command->IndirectArgs || Other.Command->IndirectArgs) return false;
		// Bucket IDs are local to cached scene storage. Reset/removal invalidates
		// source metadata before an ID can be reused, so stale/cross-scene IDs
		// cannot replace the effective-state comparison for dynamic commands.
		const bool SameBucket = CachedInfo && Other.CachedInfo && CachedInfo->Storage
			&& CachedInfo->Storage == Other.CachedInfo->Storage && StateBucketId != INDEX_NONE
			&& StateBucketId == CachedInfo->StateBucketId && Other.StateBucketId == Other.CachedInfo->StateBucketId
			&& StateBucketId == Other.StateBucketId;
		const bool SameCommand = Command == Other.Command;
		return (SameBucket || SameCommand || Command->MatchesForDynamicInstancing(*Other.Command))
			&& (SceneData.Buffers == Other.SceneData.Buffers || SceneData.Buffers->SceneData == Other.SceneData.Buffers->SceneData)
			&& DepthBias == Other.DepthBias
			&& (Pass == Other.Pass || (Pass->Vertex == Other.Pass->Vertex && Pass->Fragment == Other.Pass->Fragment));
	}

	auto BuildMeshDrawCommandRuns(std::span<const FVisibleMeshDrawCommand> Visible) -> std::vector<FMeshDrawCommandRun>
	{
		std::vector<FMeshDrawCommandRun> Runs;
		Runs.reserve(Visible.size());
		for (uint32 Index = 0; Index < Visible.size(); ++Index)
		{
			const auto& Current = Visible[Index];
			if ((!Current.bSupportsDynamicInstancing && !Current.bSupportsInstanceCulling)
				|| !Current.Command || !Current.Pass || !Current.SceneData.Buffers) continue;
			const auto& Instances = Current.SceneData.Buffers->SceneData[1];
			if (Instances && Current.SceneData.InstanceId >= Instances->GetSize() / sizeof(FInstanceSceneShaderData)) continue;
			bool bContinue = false;
			if (!Runs.empty() && Runs.back().FirstCommand + Runs.back().NumCommands == Index)
			{
				const auto& Previous = Visible[Index - 1];
				bContinue = Current.MatchesForDynamicInstancing(Previous);
			}
			if (!bContinue) Runs.push_back({Index, 0});
			++Runs.back().NumCommands;
		}
		return Runs;
	}

	auto SetupMeshDrawCommandDynamicInstancing(FRHICommandListImmediate& Commands,
		const FPreparedStaticMeshView& Prepared, FResolvedStaticMeshView& Resolved, std::span<const FMeshPassKey> RequestedPasses) -> bool
	{
		struct FPassRuns { FMeshPassKey Pass; FMeshCommandOneFrameArray Visible; };
		std::vector<FPassRuns> Passes;
		Passes.reserve(RequestedPasses.size());
		std::vector<uint32> Ids;
		Ids.reserve(Resolved.Draws.size());
		Resolved.DrawCommandSubmissions.clear();
		std::array<bool, static_cast<size_t>(EMeshPipelinePass::HitProxy) + 1> AvailablePasses{};
		for (const auto& Draw : Resolved.Draws) if (Draw.bReady)
		{
			for (const auto& [Pass, Command] : Draw.VisibleCommands) AvailablePasses[static_cast<size_t>(Pass.Pass)] = true;
			for (const auto& [Pass, Command] : Draw.Commands) AvailablePasses[static_cast<size_t>(Pass.Pass)] = true;
		}
		for (const auto Pass : RequestedPasses)
		{
			if (!AvailablePasses[static_cast<size_t>(Pass.Pass)]) continue;
			auto& Entry = Passes.emplace_back(FPassRuns{Pass, CollectMeshDrawCommands(Prepared, Resolved, Pass, false)});
			auto Runs = BuildMeshDrawCommandRuns(Entry.Visible);
			for (const auto& Run : Runs) if (Run.NumCommands > 1)
				for (uint32 Index : Run.GetDrawIndices()) Ids.push_back(Entry.Visible[Index].SceneData.InstanceId);
			Resolved.DrawCommandSubmissions[Pass] = std::make_shared<FMeshDrawCommandSubmission>(std::move(Runs), Entry.Visible.size());
		}
		if (Ids.empty()) return true;
		if (Ids.size() > UINT32_MAX / sizeof(uint32)) return false;
		if (!Resolved.GPUSceneDynamicContext) return false;
		Resolved.DynamicInstancingInstanceIds = Resolved.GPUSceneDynamicContext->UploadInstanceIds(Commands, Ids);
		if (!Resolved.DynamicInstancingInstanceIds) return false;
		uint32 Offset = 0;
		for (const auto& Entry : Passes)
		for (uint32 RunIndex = 0; RunIndex < Resolved.DrawCommandSubmissions[Entry.Pass]->Runs.size(); ++RunIndex)
		{
			const auto Run = Resolved.DrawCommandSubmissions[Entry.Pass]->Runs[RunIndex].Draws.GetDrawIndices();
			if (Run.size() < 2) continue;

			const auto& Leader = Entry.Visible[Run.front()];
			auto Bindings = std::make_shared<FMeshPassBindings>(*Leader.Pass);
			for (uint32 Index : Run)
			{
				auto Visible = Entry.Visible[Index];
				Visible.PrimitiveIdBufferOffset = Offset; Visible.NumInstances = static_cast<uint32>(Run.size());
				Visible.bSkipDraw = Index != Run.front(); Visible.InstanceBuffer = Resolved.DynamicInstancingInstanceIds;
				const auto Handle = Resolved.DrawCommandStorage->PublishVisible(Visible, Bindings);
				Resolved.Draws[Index].DynamicInstancingCommands[Entry.Pass] = {RunIndex, Handle};
			}
			Offset += static_cast<uint32>(Run.size());
		}
		return true;
	}
}
