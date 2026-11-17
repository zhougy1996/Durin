#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshVertexFactory.h"
#include "RHICommandList.h"
#include "Renderers/InstanceCulling.h"
#include "InstanceUniformShaderParameters.h"
#include "SceneView.h"
#include "Profiling/Profiling.h"

namespace Durin::RendererPrivate
{
	FParallelCommandListSet::FParallelCommandListSet(const FSceneView& View, FRHICommandList& InParent)
		: Parent(InParent), ViewRect{float(View.ViewportX), float(View.ViewportY), float(View.ViewportWidth), float(View.ViewportHeight)}
	{
		require(Parent.IsInsideRenderPass());
		const char* Setting = std::getenv("DURIN_MESH_DRAW_RECORDING");
		const std::string_view Policy = Setting ? Setting : "auto";
		if (Policy == "serial") Width = 1;
		else if (Policy == "parallel") MinDrawsPerCommandList = 1;
	}

	FParallelCommandListSet::~FParallelCommandListSet()
	{
		// Drain even on an exceptional/failed recording path before borrowed pass
		// data goes out of scope. Uninserted command payloads are simply discarded.
		for (auto& Recording : Recordings)
			if (Recording.Completion.IsValid()) require(Recording.Completion.Wait());
	}

	auto FParallelCommandListSet::NewParallelCommandList() -> FRHISubCommandList*
	{
		require(!bDispatched);
		auto Commands = std::make_unique<FRHISubCommandList>(Parent);
		Commands->SetViewport(ViewRect[0], ViewRect[1], 0, ViewRect[0] + ViewRect[2], ViewRect[1] + ViewRect[3], 1);
		Commands->SetScissor(ViewRect[0], ViewRect[1], ViewRect[2], ViewRect[3]);
		auto* Result = Commands.get();
		Recordings.push_back({std::move(Commands), {}});
		return Result;
	}

	auto FParallelCommandListSet::AddParallelCommandList(FRHISubCommandList* Commands, Tasks::FTask Completion) -> void
	{
		require(!bDispatched && Completion.IsValid());
		const auto Found = std::ranges::find_if(Recordings, [Commands](const auto& Entry) { return Entry.Commands.get() == Commands; });
		require(Found != Recordings.end() && !Found->Completion.IsValid());
		Found->Completion = std::move(Completion);
	}

	auto FParallelCommandListSet::Dispatch() -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ParallelDraw.Join");
		require(!bDispatched);
		bDispatched = true;
		bool bSucceeded = true;
		std::vector<FRHISubCommandList*> Commands;
		Commands.reserve(Recordings.size());
		for (auto& Recording : Recordings)
		{
			require(Recording.Completion.IsValid());
			const auto Wait = Recording.Completion.WaitDetailed();
			require(Wait.WaitStatus == ETaskWaitStatus::Completed);
			bSucceeded &= Wait.TaskState == ETaskState::Succeeded;
			Commands.push_back(Recording.Commands.get());
		}
		if (bSucceeded) Parent.InsertParallelRenderPass(Commands);
		return bSucceeded;
	}

	auto FParallelMeshDrawCommandPass::DispatchDraw(FParallelCommandListSet* ParallelCommandListSet, FRHICommandList& Commands,
		const FMeshDrawCommandOverrideArgs& OverrideArgs, FMeshDrawCommandPassStats* Statistics) const -> FMeshDrawCommandPassResult
	{
		return DispatchDraw(ParallelCommandListSet, Commands, 0, static_cast<uint32>(VisibleMeshDrawCommands.size()), OverrideArgs, Statistics);
	}

	auto FParallelMeshDrawCommandPass::DispatchDraw(FParallelCommandListSet* ParallelCommandListSet, FRHICommandList& Commands,
		uint32 StartIndex, uint32 NumMeshDrawCommands, const FMeshDrawCommandOverrideArgs& OverrideArgs,
		FMeshDrawCommandPassStats* Statistics) const -> FMeshDrawCommandPassResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ParallelDraw.Dispatch");
		if (StartIndex > VisibleMeshDrawCommands.size() || NumMeshDrawCommands > VisibleMeshDrawCommands.size() - StartIndex)
			return {.bComplete = false};
		const uint32 Workers = ParallelCommandListSet ? GetTaskSchedulerWorkerCount() : 0;
		if (!ParallelCommandListSet || Workers < 2 || ParallelCommandListSet->Width < 2
			|| NumMeshDrawCommands <= std::max(1u, ParallelCommandListSet->MinDrawsPerCommandList))
			return Draw(Commands, StartIndex, NumMeshDrawCommands, OverrideArgs, nullptr, Statistics);
		require(&ParallelCommandListSet->GetParentCommandList() == &Commands);
		require(bFinalized);
		const uint32 MinDraws = std::max(1u, ParallelCommandListSet->MinDrawsPerCommandList);
		const uint32 NumTasks = std::min({Workers, ParallelCommandListSet->Width, 1u + (NumMeshDrawCommands - 1) / MinDraws});
		const uint32 DrawsPerTask = 1u + (NumMeshDrawCommands - 1) / NumTasks;
		struct FTaskOutput { FMeshDrawCommandPassResult Result; FMeshDrawCommandPassStats Statistics; };
		// Output and handle storage is fixed before any callback borrows it.
		std::vector<FTaskOutput> Outputs(NumTasks);
		struct FDrainBeforeOutputs
		{
			std::vector<Tasks::FTask> Tasks;
			~FDrainBeforeOutputs() { for (auto& Task : Tasks) require(Task.Wait()); }
		} Drain;
		Drain.Tasks.reserve(NumTasks);
		for (uint32 TaskIndex = 0, First = 0; First < NumMeshDrawCommands; ++TaskIndex)
		{
			const uint32 Count = std::min(DrawsPerTask, NumMeshDrawCommands - First);
			auto* Child = ParallelCommandListSet->NewParallelCommandList();
			Tasks::FTaskExecutionOptions Options;
			Options.Prerequisites = ParallelCommandListSet->GetPrereqs();
			auto Task = Tasks::LaunchIndependentTask("Renderer.DrawVisibleMeshCommands",
				[this, Child, First = StartIndex + First, Count, OverrideArgs, Output = &Outputs[TaskIndex], bStats = Statistics != nullptr] {
					DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ParallelDraw.Record");
					Output->Result = Draw(*Child, First, Count, OverrideArgs, nullptr, bStats ? &Output->Statistics : nullptr);
					Child->FinishRecording();
				}, Options);
			Drain.Tasks.push_back(Task);
			ParallelCommandListSet->AddParallelCommandList(Child, std::move(Task));
			First += Count;
		}
		FMeshDrawCommandPassResult Result;
		if (!ParallelCommandListSet->Dispatch())
		{
			for (const auto& Visible : std::span(VisibleMeshDrawCommands).subspan(StartIndex, NumMeshDrawCommands))
				Result.Record(Visible.NumSourceDraws, false, 0, false);
			return Result;
		}
		for (auto& Output : Outputs)
		{
			const auto& Part = Output.Result;
			Result.Attempted += Part.Attempted; Result.Successful += Part.Successful; Result.Rejected += Part.Rejected;
			Result.NativeSubmissions += Part.NativeSubmissions; Result.IndirectSubmissions += Part.IndirectSubmissions;
			Result.bComplete &= Part.bComplete; Result.bRenderedGeometry |= Part.bRenderedGeometry;
			if (Statistics) Statistics->DrawData.insert(Statistics->DrawData.end(), Output.Statistics.DrawData.begin(), Output.Statistics.DrawData.end());
		}
		return Result;
	}

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
			|| !Pipeline->GetInitializer().VertexDeclaration || !Other.Pipeline->GetInitializer().VertexDeclaration) return false;
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

	auto FVisibleMeshDrawCommand::MatchesForDynamicInstancing(const FVisibleMeshDrawCommand& Other) const -> bool
	{
		if (!Command || !Other.Command || !Pass || !Other.Pass
			|| (CachedInfo && CachedInfo->CommandIndex == INDEX_NONE)
			|| (Other.CachedInfo && Other.CachedInfo->CommandIndex == INDEX_NONE)) return false;
		// Invalidated cached commands retain their value memory, not the borrowed
		// pipeline/resources. Reject their source metadata before dereferencing it.
		if (!Command->bFinalized || !Other.Command->bFinalized || !Command->Pipeline || !Other.Command->Pipeline
			|| !Command->Pipeline->GetInitializer().VertexDeclaration || !Other.Command->Pipeline->GetInitializer().VertexDeclaration
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
			&& DepthBias == Other.DepthBias
			&& (Pass == Other.Pass || (Pass->Vertex == Other.Pass->Vertex && Pass->Fragment == Other.Pass->Fragment));
	}

	namespace
	{
		auto SameSceneDomain(const FGPUScenePrimitiveData& A, const FGPUScenePrimitiveData& B) -> bool
		{ return A.Buffers && B.Buffers && (A.Buffers == B.Buffers || A.Buffers->SceneData == B.Buffers->SceneData); }
	}
	auto BuildMeshDrawCommandCullingPayloads(const FPreparedStaticMeshView& Prepared,
		const FResolvedStaticMeshView& Resolved, FMeshPassKey Pass) -> std::vector<FMeshDrawCommandCullingPayload>
	{
		std::vector<FMeshDrawCommandCullingPayload> Sources(Resolved.Draws.size());
		for (uint32 Index = 0; Index < Prepared.GetNumSections(); ++Index)
		{
			const auto& Draw = Prepared.GetDraw(Index);
			const auto* Primitive = Prepared.GetPrimitive(Draw);
			if (!Primitive || !Draw.Command || Draw.ResolvedIndex >= Sources.size()) continue;
			auto& Source = Sources[Draw.ResolvedIndex];
			if (Draw.PrimitiveIndex < Resolved.GPUScenePrimitives.size()) Source.SceneData = Resolved.GPUScenePrimitives[Draw.PrimitiveIndex];
			Source.bSupportsDynamicInstancing = Index < Prepared.Opaque.size() + Prepared.Masked.size()
				&& Primitive->VertexDomain == EVertexDeformationDomain::Local && Draw.Command->Geometry.InstanceCount == 1
				&& Primitive->CollectedBinding && std::ranges::none_of(Primitive->CollectedBinding->DeclarationElements,
					[](const auto& Element) { return Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance; });
			Source.bSupportsInstanceCulling = Source.bSupportsDynamicInstancing && Draw.Command->Geometry.bIndexed
				&& (Pass.Pass != EMeshPipelinePass::GBuffer || Draw.Command->bSupportsGBuffer) && Primitive->WorldBounds.bIsValid
				&& Math::IsFinite(Primitive->WorldBounds.Min) && Math::IsFinite(Primitive->WorldBounds.Max);
		}
		return Sources;
	}
	auto BuildMeshDrawCommandRuns(std::span<const FVisibleMeshDrawCommand> Visible,
		std::span<const FMeshDrawCommandCullingPayload> Sources) -> std::vector<FMeshDrawCommandRun>
	{
		std::vector<FMeshDrawCommandRun> Runs;
		Runs.reserve(Visible.size());
		for (uint32 Index = 0; Index < Visible.size(); ++Index)
		{
			const auto& Current = Visible[Index];
			if (Current.SourceDrawIndex >= Sources.size() || !Current.Command || !Current.Pass) continue;
			const auto& Source = Sources[Current.SourceDrawIndex];
			if ((!Source.bSupportsDynamicInstancing && !Source.bSupportsInstanceCulling) || !Source.SceneData.Buffers) continue;
			const auto& Instances = Source.SceneData.Buffers->SceneData[1];
			if (Instances && Source.SceneData.InstanceId >= Instances->GetSize() / sizeof(FInstanceSceneShaderData)) continue;
			bool bContinue = false;
			if (!Runs.empty() && Runs.back().FirstCommand + Runs.back().NumCommands == Index)
			{
				const auto& Previous = Visible[Index - 1];
				bContinue = Previous.SourceDrawIndex < Sources.size()
					&& uint64(Previous.SourceDrawIndex) + Previous.NumSourceDraws == Current.SourceDrawIndex
					&& SameSceneDomain(Source.SceneData, Sources[Previous.SourceDrawIndex].SceneData)
					&& Current.BasePass == Previous.BasePass && Current.MatchesForDynamicInstancing(Previous);
			}
			if (!bContinue) Runs.push_back({Index, 0});
			++Runs.back().NumCommands;
		}
		return Runs;
	}
	auto FMeshDrawCommandPassResult::Record(uint32 NumSourceDraws, bool bSuccess, uint32 NativeCalls, bool bIndirect) -> void
	{
		Attempted += NumSourceDraws;
		if (bSuccess) { Successful += NumSourceDraws; bRenderedGeometry = true; }
		else { Rejected += NumSourceDraws; bComplete = false; }
		NativeSubmissions += NativeCalls;
		if (bIndirect) IndirectSubmissions += NativeCalls;
	}
	auto FParallelMeshDrawCommandPass::SetupDrawCommands(FInstanceCullingManager& Manager, uint32 ViewId, std::span<const FMeshDrawCommandCullingPayload> Sources,
		size_t* Overflow, size_t Budget, FInstanceCullingAdmissionDiagnostics* Diagnostics) -> bool
	{
		require((bFinalized || VisibleMeshDrawCommands.empty()) && !InstanceCullingContext);
		bFinalized = true;
		InstanceCullingContext = Manager.CreateContext(ViewId, Pass);
		const bool Selected = InstanceCullingContext->SetupDrawCommands(VisibleMeshDrawCommands, Sources, Overflow, Budget, Diagnostics);
		std::ranges::fill(SourceCommandIndices, INDEX_NONE_U32);
		for (uint32 Index = 0; Index < VisibleMeshDrawCommands.size(); ++Index)
		{
			const auto& Command = VisibleMeshDrawCommands[Index];
			for (uint32 Source = Command.SourceDrawIndex; Source < SourceCommandIndices.size()
				&& Source - Command.SourceDrawIndex < Command.NumSourceDraws; ++Source) SourceCommandIndices[Source] = Index;
		}
		uint32 Cursor = 0;
		for (uint32 Bucket = 0; Bucket < BasePassRanges.size(); ++Bucket)
		{
			const uint32 First = Cursor;
			while (Cursor < VisibleMeshDrawCommands.size() && static_cast<uint32>(VisibleMeshDrawCommands[Cursor].BasePass) == Bucket) ++Cursor;
			BasePassRanges[Bucket] = {First, Cursor - First};
		}
		return Selected;
	}
	auto FParallelMeshDrawCommandPass::Draw(FRHICommandList& Commands, const FMeshDrawCommandOverrideArgs& OverrideArgs,
		FMeshDrawCommandStateCache* Cache, FMeshDrawCommandPassStats* Statistics) const -> FMeshDrawCommandPassResult
	{ return Draw(Commands, 0, static_cast<uint32>(VisibleMeshDrawCommands.size()), OverrideArgs, Cache, Statistics); }
	auto FParallelMeshDrawCommandPass::Draw(FRHICommandList& Commands, uint32 StartIndex, uint32 NumMeshDrawCommands,
		const FMeshDrawCommandOverrideArgs& OverrideArgs, FMeshDrawCommandStateCache* StateCache, FMeshDrawCommandPassStats* Statistics) const -> FMeshDrawCommandPassResult
	{
		require(bFinalized || VisibleMeshDrawCommands.empty());
		FMeshDrawCommandPassResult Result;
		if (RenderFrame != GRenderFrameCounterRenderThread && !VisibleMeshDrawCommands.empty())
		{ Result.bComplete = false; return Result; }
		FMeshDrawCommandStateCache LocalCache; auto* Cache = StateCache ? StateCache : &LocalCache;
		if (StartIndex > VisibleMeshDrawCommands.size() || NumMeshDrawCommands > VisibleMeshDrawCommands.size() - StartIndex)
		{ Result.bComplete = false; return Result; }
		if (InstanceCullingContext) return InstanceCullingContext->SubmitDrawCommands(VisibleMeshDrawCommands, OverrideArgs, StartIndex, NumMeshDrawCommands, Commands, Cache, Statistics);
		for (uint32 Index = StartIndex; Index < StartIndex + NumMeshDrawCommands; ++Index)
		{
			const uint32 Before = Cache->DrawCalls;
			const auto& Visible = VisibleMeshDrawCommands[Index];
			const bool Submitted = Visible.SubmitDraw(Commands, Cache);
			Result.Record(Visible.NumSourceDraws, Submitted, Cache->DrawCalls - Before, false);
			if (Statistics) Statistics->Record(Visible.SourceDrawIndex, Visible.NumSourceDraws, Submitted, Cache->DrawCalls - Before, false);
		}
		return Result;
	}

	auto SetupMeshDrawCommands(FRHICommandListImmediate& Commands,
		const FPreparedStaticMeshView& Prepared, FResolvedStaticMeshView& Resolved, bool bAllowDynamicInstancing) -> bool
	{
		std::vector<uint32> MergedIds;
		std::vector<const FPreparedStaticMeshDraw*> SourceDraws(Resolved.Draws.size());
		const auto Sources = BuildMeshDrawCommandCullingPayloads(Prepared, Resolved, {EMeshPipelinePass::Forward});
		std::vector<EMeshBasePass> SourcePasses(Resolved.Draws.size());
		Resolved.GPUSceneBuffers.reset();
		for (const auto& Primitive : Resolved.GPUScenePrimitives) if (Primitive.Buffers)
		{
			if (!Resolved.GPUSceneBuffers) Resolved.GPUSceneBuffers = Primitive.Buffers;
			else if (Resolved.GPUSceneBuffers->SceneData != Primitive.Buffers->SceneData
				|| Resolved.GPUSceneBuffers->InstanceIds != Primitive.Buffers->InstanceIds) return false;
		}
		for (uint32 Index = 0; Index < Prepared.GetNumSections(); ++Index)
		{
			const auto& Draw = Prepared.GetDraw(Index);
			SourceDraws[Draw.ResolvedIndex] = &Draw;
			SourcePasses[Draw.ResolvedIndex] = Index < Prepared.Opaque.size() ? EMeshBasePass::Opaque
				: Index < Prepared.Opaque.size() + Prepared.Masked.size() ? EMeshBasePass::Masked : EMeshBasePass::Translucent;
		}
		for (auto& [PassKey, Entry] : Resolved.MeshDrawCommandPasses)
		{
			auto& Target = *Entry;
			const auto Pass = PassKey.Pass;
			if (Target.VisibleMeshDrawCommands.empty()) continue;
			require(!Target.bFinalized);
			auto& Visible = Target.VisibleMeshDrawCommands;
			std::ranges::sort(Visible, {}, &FVisibleMeshDrawCommand::SourceDrawIndex);
			Target.SourceCommandIndices.assign(Resolved.Draws.size(), INDEX_NONE_U32);
			for (auto& Item : Visible)
			{
				if (Item.SourceDrawIndex >= SourceDraws.size() || !SourceDraws[Item.SourceDrawIndex]) { Item.Command = nullptr; continue; }
				const auto& Draw = *SourceDraws[Item.SourceDrawIndex];
				const auto* Primitive = Prepared.GetPrimitive(Draw);
				if (!Primitive || !Resolved.IsReady(Draw)) { Item.Command = nullptr; continue; }
				Item.BasePass = SourcePasses[Item.SourceDrawIndex];
				Item.bSourceReady = (Pass == EMeshPipelinePass::GBuffer || Pass == EMeshPipelinePass::ShadowDepth)
					|| (Primitive->PrimitiveId != InvalidPrimitiveComponentId && Primitive->CollectedBinding
					&& Primitive->CollectedBinding->Declaration && Draw.Command->Geometry.ElementCount != 0
					&& std::isfinite(Draw.TranslucentSortDepth) && Draw.Command->PipelineKey.Material == Draw.Command->Material.PlanningPassIdentity
					&& Draw.Command->SortKey.Pipeline[0] == static_cast<uint32>(Item.BasePass) && Draw.Command->Pass == Item.BasePass);
				if (const auto& Owner = Sources[Item.SourceDrawIndex].SceneData.Buffers; Owner
					&& std::ranges::find(Target.SceneDataOwners, Owner) == Target.SceneDataOwners.end()) Target.SceneDataOwners.push_back(Owner);
			}
			std::erase_if(Visible, [](const auto& Item) { return !Item.Command; });
			// Compact in place, retaining source indices solely for provider statistics.
			size_t Output = 0;
			for (size_t First = 0; First < Visible.size();)
			{
				size_t End = First + 1;
				if (bAllowDynamicInstancing && Sources[Visible[First].SourceDrawIndex].bSupportsDynamicInstancing)
					while (End < Visible.size() && Sources[Visible[End].SourceDrawIndex].bSupportsDynamicInstancing
						&& Visible[End].BasePass == Visible[First].BasePass
						&& Visible[End].SourceDrawIndex == Visible[End - 1].SourceDrawIndex + 1
						&& SameSceneDomain(Sources[Visible[End].SourceDrawIndex].SceneData, Sources[Visible[End - 1].SourceDrawIndex].SceneData)
						&& Visible[End].MatchesForDynamicInstancing(Visible[End - 1])) ++End;
				auto Leader = Visible[First];
				Leader.NumSourceDraws = static_cast<uint32>(End - First);
				for (size_t Index = First; Index < End; ++Index)
				{
					Target.SourceCommandIndices[Visible[Index].SourceDrawIndex] = static_cast<uint32>(Output);
					Leader.bSourceReady &= Visible[Index].bSourceReady;
				}
				if (End - First > 1)
				{
					Leader.NumInstances = static_cast<uint32>(End - First);
					Leader.PrimitiveIdBufferOffset = static_cast<uint32>(MergedIds.size());
					for (size_t Index = First; Index < End; ++Index)
						MergedIds.push_back(Sources[Visible[Index].SourceDrawIndex].SceneData.InstanceId);
				}
				Visible[Output++] = Leader; First = End;
			}
			Visible.resize(Output);
			uint32 Cursor = 0;
			for (uint32 Bucket = 0; Bucket < Target.BasePassRanges.size(); ++Bucket)
			{
				const uint32 First = Cursor;
				while (Cursor < Visible.size() && static_cast<uint32>(Visible[Cursor].BasePass) == Bucket) ++Cursor;
				Target.BasePassRanges[Bucket] = {First, Cursor - First};
			}
			Target.bFinalized = true;
		}
		if (MergedIds.empty()) return true;
		if (MergedIds.size() > UINT32_MAX / sizeof(uint32) || !Resolved.GPUSceneDynamicContext) return false;
		Resolved.DynamicInstancingInstanceIds = Resolved.GPUSceneDynamicContext->UploadInstanceIds(Commands, MergedIds);
		if (!Resolved.DynamicInstancingInstanceIds) return false;
		for (auto& [Pass, Target] : Resolved.MeshDrawCommandPasses)
			for (auto& Item : Target->VisibleMeshDrawCommands)
				if (Item.NumInstances > 1) { Target->InstanceIdBuffer = Resolved.DynamicInstancingInstanceIds; Item.InstanceBuffer = Target->InstanceIdBuffer; }
		return true;
	}
}
