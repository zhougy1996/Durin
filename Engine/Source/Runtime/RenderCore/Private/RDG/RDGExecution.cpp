#include "RDGBuilderInternal.h"
#include "Misc/Time.h"
#include "Profiling/Profiling.h"
#include <format>
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "Threading/TaskComposition.h"
#include <deque>

namespace Durin::RDGPrivate
{
	namespace
	{
		// Execution-local physical data; never stored in the immutable logical plan.
		struct FPreparedTransitions final
		{
			std::vector<FRHIBufferTransition> Buffers;
			std::vector<FRHITextureTransition> Textures;
			std::vector<bool> TransferredBuffers, TransferredTextures;
		};
		struct FPreparedBarrierBatch final
		{
			size_t FirstBuffer = 0, NumBuffers = 0;
			size_t FirstTexture = 0, NumTextures = 0;
		};
		// Resolve every barrier before recording callbacks or emitting any graph work.
		auto PrepareBarrierBatch(
			const FRDGBarrierBatch& Batch, std::span<const FGraphResourceBacking> Backings,
			FPreparedTransitions& Scratch) -> FPreparedBarrierBatch;
		auto RecordBarrierBatch(FRHICommandListBase& CommandList,
			const FPreparedBarrierBatch& Batch, const FPreparedTransitions& Prepared) -> void;
		auto TextureBackingIsCompatible(const FRHITextureDesc& Actual,
			const FRHITextureDesc& Required) -> bool;
		auto BufferBackingIsCompatible(const FRHIBufferDesc& Actual,
			const FRHIBufferDesc& Required) -> bool;

		auto PrepareBarrierBatch(
			const FRDGBarrierBatch& Batch, std::span<const FGraphResourceBacking> Backings,
			FPreparedTransitions& Scratch) -> FPreparedBarrierBatch
		{
			const FPreparedBarrierBatch Result{Scratch.Buffers.size(), Batch.GetBufferTransitions().size(),
				Scratch.Textures.size(), Batch.GetTextureTransitions().size()};
			for (const auto& Transition : Batch.GetBufferTransitions())
			{
				check(Transition.ResourceId < Backings.size());
				Scratch.Buffers.push_back({Backings[Transition.ResourceId].Buffer.GetReference(),
					Transition.Offset, Transition.Size, Transition.ExpectedBefore,
					Transition.RequiredAfter, Transition.bDiscardContents});
			}
			for (const auto& Transition : Batch.GetTextureTransitions())
			{
				check(Transition.ResourceId < Backings.size());
				Scratch.Textures.push_back({Backings[Transition.ResourceId].Texture.GetReference(),
					Transition.Range, Transition.ExpectedBefore,
					Transition.RequiredAfter, Transition.bDiscardContents});
			}
			return Result;
		}

		auto RecordBarrierBatch(FRHICommandListBase& CommandList,
			const FPreparedBarrierBatch& Batch, const FPreparedTransitions& Prepared) -> void
		{
			auto Record = [](size_t First, size_t Count, const auto& Transferred, auto&& Emit) {
				const size_t End = First + Count;
				while (First < End)
				{
					while (First < End && !Transferred.empty() && Transferred[First]) ++First;
					const size_t Begin = First;
					while (First < End && (Transferred.empty() || !Transferred[First])) ++First;
					if (First != Begin) Emit(Begin, First - Begin);
				}
			};
			Record(Batch.FirstBuffer, Batch.NumBuffers, Prepared.TransferredBuffers,
				[&](size_t First, size_t Count) { CommandList.TransitionBuffers(std::span{Prepared.Buffers}.subspan(First, Count)); });
			Record(Batch.FirstTexture, Batch.NumTextures, Prepared.TransferredTextures,
				[&](size_t First, size_t Count) { CommandList.TransitionTextures(std::span{Prepared.Textures}.subspan(First, Count)); });
		}

		auto TextureBackingIsCompatible(const FRHITextureDesc& Actual,
			const FRHITextureDesc& Required) -> bool
		{
			FRHITextureDesc NormalizedActual = Actual;
			NormalizedActual.Flags = Required.Flags;
			return EnumHasAllFlags(Actual.Flags, Required.Flags)
				&& TextureDescriptionsEqual(NormalizedActual, Required);
		}

		auto BufferBackingIsCompatible(const FRHIBufferDesc& Actual,
			const FRHIBufferDesc& Required) -> bool
		{
			FRHIBufferDesc NormalizedActual = Actual;
			NormalizedActual.Usage = Required.Usage;
			return EnumHasAllFlags(Actual.Usage, Required.Usage)
				&& BufferDescriptionsEqual(NormalizedActual, Required);
		}

		}

}

namespace Durin
{
	using namespace RDGPrivate;

	// Physical preparation lives only for this execution. The compiled plan stays logical.
	struct FRDGBuilder::FExecutionContext final
	{
		FPreparedTransitions PreparedTransitions;
		std::vector<FPreparedBarrierBatch> PreparedPassBarriers;
		FPreparedBarrierBatch PreparedEpilogue;
		const FRHIQueueCapabilities* Queues = nullptr;
		bool bExplicitSubmissions = false;
		bool bAsync = false;
		using FTransfers = std::vector<std::shared_ptr<FRHIQueueTransfer>>;
		std::vector<FTransfers> Acquires, Releases;
		FTransfers InitialReleases;
		std::vector<bool> WaitForInitial;
		std::vector<std::vector<uint32>> Predecessors;

		auto PhysicalQueue(ERDGQueueAssignment Queue) const -> FRHIQueueId
		{
			return bAsync && Queue == ERDGQueueAssignment::AsyncCompute
				? Queues->Compute : Queues->Graphics;
		}
	};

	auto FRDGBuilder::Execute(FRHICommandListImmediate& CommandList,
		FRDGAllocator* Allocator) -> FRDGExecutionResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RDG.Execute");
		if (State->Lifecycle != ERDGBuilderState::Building)
			return std::unexpected(ERDGStateError::BuilderConsumed);
		State->Lifecycle = ERDGBuilderState::Compiling;
		struct FFailureGuard
		{
			ERDGBuilderState& State;
			~FFailureGuard()
			{
				if (State != ERDGBuilderState::Recorded) State = ERDGBuilderState::Failed;
			}
		} Guard{State->Lifecycle};
		State->ExecutionResult = std::unexpected(FRDGExecutionError{
			FRDGCompileError{ERDGStateError::CompilationIncomplete}});
		if (auto Result = Compile(); !Result)
		{
			State->ExecutionResult = std::unexpected(FRDGExecutionError{std::move(Result.error())});
			return *State->ExecutionResult;
		}
		State->Lifecycle = ERDGBuilderState::Preparing;
		State->ExecutionResult = std::unexpected(FRDGExecutionError{
			FRDGPreparationError{ERDGStateError::PreparationIncomplete}});
		if (auto Result = Record(CommandList, Allocator); !Result)
		{
			State->ExecutionResult = std::unexpected(FRDGExecutionError{std::move(Result.error())});
			return *State->ExecutionResult;
		}
		State->Lifecycle = ERDGBuilderState::Recorded;
		State->ExecutionResult = FRDGExecutionResult{};
		return *State->ExecutionResult;
	}

	auto FRDGBuilder::Record(
		FRHICommandListImmediate& CommandList, FRDGAllocator* Allocator) -> FRDGPreparationResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RDG.Record");
		FScopedMicrosecondTimer ExecuteTimer(State->ExecuteMicroseconds);
		FExecutionContext Context;
		{
			FScopedMicrosecondTimer PreparationTimer(State->Phases.PreparationMicroseconds);
			if (auto Result = AllocateResources(Allocator); !Result) return Result;
			if (auto Result = PrepareExecution(Context, Allocator); !Result) return Result;
		}

		FScopedMicrosecondTimer RecordingTimer(State->Phases.RecordingMicroseconds);
		State->Lifecycle = ERDGBuilderState::Recording;
		State->ExecutionResult = std::unexpected(FRDGExecutionError{ERDGStateError::RecordingIncomplete});
		if (auto Result = RecordPasses(CommandList, Context); !Result) return Result;
		PublishExtractions();
		return {};
	}

	auto FRDGBuilder::AllocateResources(FRDGAllocator* Allocator) -> FRDGPreparationResult
	{
		if (Allocator != nullptr && !Compiled->AllocationRequests.empty())
		{
			FRDGAllocatedResources Candidate(
				static_cast<uint32>(Compiled->Resources.size()));
			std::span<const FRDGAllocationRequest> Requests = Compiled->AllocationRequests;
			std::vector<FRDGAllocationRequest> AsyncRequests;
			if (State->bAsyncComputeEnabled && GDynamicRHI
				&& GDynamicRHI->RHIGetQueueCapabilities().bIndependentCompute
				&& Allocator->SupportsAsyncCompute())
			{
				State->AllocationRetirement = std::make_shared<FRDGAllocationRetirement>();
				AsyncRequests = Compiled->AllocationRequests;
				for (auto& Request : AsyncRequests) Request.Retirement = State->AllocationRetirement;
				Requests = AsyncRequests;
			}
			auto AllocationResult = [&] {
				DURIN_PROFILE_CPU_ZONE_NAMED("RDG.AllocateResources");
				auto Result = Allocator->Allocate(Requests, Candidate);
				DURIN_PROFILE_CPU_ZONE_TEXT(std::format(
					"requests={} reuse_hits={} reuse_misses={} failures={} success={}",
					Requests.size(), Candidate.Statistics.ReuseHits,
					Candidate.Statistics.ReuseMisses, Candidate.Statistics.Failures,
					Result.has_value()));
				return Result;
			}();
			Compiled->AllocationStatistics = Candidate.Statistics;
			if (!AllocationResult.has_value())
			{
				return AllocationResult;
			}
			for (const FRDGAllocationRequest& Request : Compiled->AllocationRequests)
			{
				const bool bReady = Request.Kind == ERDGResourceKind::Texture
					? static_cast<bool>(Candidate.Textures[Request.ResourceId])
					: static_cast<bool>(Candidate.Buffers[Request.ResourceId]);
				if (!bReady)
				{
					return std::unexpected(FRDGMissingAllocationError{Request.ResourceId});
				}
				if (Request.Kind == ERDGResourceKind::Texture)
				{
					const FRHITextureDesc Actual = DescribeTexture(
						*Candidate.Textures[Request.ResourceId]);
					if (!TextureBackingIsCompatible(Actual, Request.TextureDesc))
					{
						return std::unexpected(FRDGTextureAllocationError{.ResourceId = Request.ResourceId,
								.Expected = Request.TextureDesc,
								.Actual = Actual});
					}
				}
				else if (!BufferBackingIsCompatible(
					Candidate.Buffers[Request.ResourceId]->GetDesc(),
					Request.BufferDesc))
				{
					return std::unexpected(FRDGBufferAllocationError{.ResourceId = Request.ResourceId,
							.Expected = Request.BufferDesc,
							.Actual = Candidate.Buffers[Request.ResourceId]->GetDesc()});
				}
			}
			for (const FRDGAllocationRequest& Request : Compiled->AllocationRequests)
			{
				auto& Resource = Compiled->Backings[Request.ResourceId];
				Resource.AllocationDisposition =
					Candidate.AllocationDispositions[Request.ResourceId];
				Resource.PhysicalAllocationId =
					Candidate.AllocationIds[Request.ResourceId];
				if (Diagnostics)
				{
					auto& Capture = Diagnostics->Resources[Request.ResourceId];
					Capture.AllocationDisposition = Resource.AllocationDisposition;
					Capture.PhysicalAllocationId = Resource.PhysicalAllocationId;
				}
				if (Request.Kind == ERDGResourceKind::Texture)
					Resource.Texture = std::move(
						Candidate.Textures[Request.ResourceId]
					);
				else
					Resource.Buffer = std::move(
						Candidate.Buffers[Request.ResourceId]
					);
			}
		}
		else if (!Compiled->AllocationRequests.empty())
		{
			return std::unexpected(ERDGPreparationError::AllocatorMissing);
		}
		return {};
	}

	auto FRDGBuilder::PrepareExecution(FExecutionContext& Context,
		FRDGAllocator* Allocator) -> FRDGPreparationResult
	{
		Context.PreparedPassBarriers.reserve(Compiled->Passes.size());
		for (const auto& Pass : Compiled->Passes)
			Context.PreparedPassBarriers.push_back(PrepareBarrierBatch(Pass.Barriers, Compiled->Backings, Context.PreparedTransitions));
		Context.PreparedEpilogue = PrepareBarrierBatch(Compiled->FinalBarriers, Compiled->Backings, Context.PreparedTransitions);
		Context.Queues = GDynamicRHI ? &GDynamicRHI->RHIGetQueueCapabilities() : nullptr;
		Context.bExplicitSubmissions = Context.Queues && !Context.Queues->Queues.empty();
		Context.bAsync = State->bAsyncComputeEnabled && Context.bExplicitSubmissions
			&& Context.Queues->bIndependentCompute && Context.Queues->Compute != Context.Queues->Graphics
			&& (Compiled->AllocationRequests.empty() || (Allocator && Allocator->SupportsAsyncCompute()));
		Context.Acquires.resize(Compiled->ExecutionPlan.Batches.size());
		Context.Releases.resize(Context.Acquires.size());
		Context.WaitForInitial.resize(Context.Acquires.size(), false);
		if (Context.bAsync)
		{
			Context.PreparedTransitions.TransferredBuffers.resize(Context.PreparedTransitions.Buffers.size(), false);
			Context.PreparedTransitions.TransferredTextures.resize(Context.PreparedTransitions.Textures.size(), false);
			for (const auto& Handoff : Compiled->ExecutionPlan.Handoffs)
			{
				const auto& Consumer = Compiled->ExecutionPlan.Batches[Handoff.Consumer.Index];
				if (Handoff.SourceQueue == Consumer.Queue) continue;
				FRHIQueueTransferDesc Desc{.Source = Context.PhysicalQueue(Handoff.SourceQueue), .Destination = Context.PhysicalQueue(Consumer.Queue)};
				const auto& Barrier = Consumer.bEpilogue ? Context.PreparedEpilogue : Context.PreparedPassBarriers[Handoff.ConsumerPass];
				if (Handoff.bTexture)
				{
					const size_t Index = Barrier.FirstTexture + Handoff.TransitionIndex;
					Desc.Textures.push_back(Context.PreparedTransitions.Textures[Index]);
					Context.PreparedTransitions.TransferredTextures[Index] = true;
				}
				else
				{
					const size_t Index = Barrier.FirstBuffer + Handoff.TransitionIndex;
					Desc.Buffers.push_back(Context.PreparedTransitions.Buffers[Index]);
					Context.PreparedTransitions.TransferredBuffers[Index] = true;
				}
				auto Transfer = GDynamicRHI->RHICreateQueueTransfer(Desc);
				if (!Transfer) return std::unexpected(ERDGPreparationError::QueueTransferFailed);
				Context.Acquires[Consumer.Id.Index].push_back(Transfer);
				const auto Producer = std::ranges::find_if(Handoff.Producers, [&](const auto Id) {
					return Compiled->ExecutionPlan.Batches[Id.Index].Queue == Handoff.SourceQueue;
				});
				if (Producer != Handoff.Producers.end()) Context.Releases[Producer->Index].push_back(std::move(Transfer));
				else
				{
					require(Handoff.SourceQueue == ERDGQueueAssignment::Graphics);
					Context.InitialReleases.push_back(std::move(Transfer));
					Context.WaitForInitial[Consumer.Id.Index] = true;
				}
			}
		}
		Context.Predecessors.resize(Compiled->ExecutionPlan.Batches.size());
		if (Context.bExplicitSubmissions)
		{
			State->SubmissionSyncPoints.resize(Compiled->ExecutionPlan.Batches.size());
			for (const auto& Edge : Compiled->ExecutionPlan.Dependencies)
				Context.Predecessors[Edge.After.Index].push_back(Edge.Before.Index);
			for (auto& Inputs : Context.Predecessors)
			{
				std::ranges::sort(Inputs);
				Inputs.erase(std::unique(Inputs.begin(), Inputs.end()), Inputs.end());
			}
		}
		return {};
	}

	auto FRDGBuilder::RecordPasses(FRHICommandListImmediate& CommandList,
		const FExecutionContext& Context) -> FRDGPreparationResult
	{
		uint32 DeclarationCount = 0;
		for (const auto& Pass : Compiled->Passes) DeclarationCount = std::max(DeclarationCount, Pass.DeclarationIndex + 1);
		std::vector<uint32> DeclarationToCompiled(DeclarationCount, UINT32_MAX);
		for (uint32 Index = 0; Index < Compiled->Passes.size(); ++Index)
			DeclarationToCompiled[Compiled->Passes[Index].DeclarationIndex] = Index;
		std::vector<uint32> PrerequisiteEnd(Compiled->Passes.size(), 0);
		for (const auto& Edge : Compiled->Dependencies)
		{
			const uint32 Before = DeclarationToCompiled[Edge.BeforePass], After = DeclarationToCompiled[Edge.AfterPass];
			require(Before != UINT32_MAX && After != UINT32_MAX && Before < After);
			PrerequisiteEnd[After] = std::max(PrerequisiteEnd[After], Before + 1);
		}
		auto RecordOwned = [this](uint32 Index) {
			FRHICommandList Recorded;
			const auto& Pass = Compiled->Passes[Index];
			const auto& Runtime = Compiled->RuntimePasses[Index];
			const FRDGPassResources Resources(*this, Index);
			const FRDGParameterResolver Resolver(Resources, Runtime.ParameterLayout,
				Runtime.OptionalAliases, Runtime.Parameters, Pass.Name, Pass.Type);
			(*Runtime.RecordingExecute)(Recorded, Resolver);
			Recorded.FinishRecording();
			return Recorded;
		};
		std::deque<FRHICommandList> ReadyRecordings;
		auto PrepareRecordingWave = [&](uint32 First) -> bool {
			auto Eligible = [&](uint32 Index) {
				const auto& Runtime = Compiled->RuntimePasses[Index];
				return Runtime.RecordingPolicy == ERDGRecordingPolicy::Parallel
					&& Runtime.RecordingExecute && *Runtime.RecordingExecute;
			};
			uint32 End = First + 1;
			if (IsTaskSchedulerRunning() && Eligible(First))
				while (End < Compiled->Passes.size() && End - First < 8
					&& Eligible(End) && PrerequisiteEnd[End] <= First) ++End;
			if (End == First + 1)
			{
				ReadyRecordings.push_back(RecordOwned(First));
				return true;
			}
			struct FPendingRecordings
			{
				std::vector<Tasks::TTask<FRHICommandList>> Work;
				~FPendingRecordings()
				{
					for (auto& Task : Work) if (Task.IsValid()) Tasks::Cancel(Task.GetCompletion());
					for (auto& Task : Work) if (Task.IsValid()) require(Task.Wait().WaitStatus == ETaskWaitStatus::Completed);
				}
			} Pending;
			// Allocate handle storage before launching work that borrows this graph.
			// A vector growth failure must not orphan an already-running callback.
			Pending.Work.reserve(End - First);
			for (uint32 Index = First; Index < End; ++Index)
				Pending.Work.push_back(Tasks::LaunchIndependentTask("RDG.RecordPass", [RecordOwned, Index] { return RecordOwned(Index); }));
			for (auto& Task : Pending.Work)
			{
				const auto Wait = Task.Wait();
				require(Wait.WaitStatus == ETaskWaitStatus::Completed);
				if (Wait.TaskState != ETaskState::Succeeded)
				{
					ReadyRecordings.clear();
					return false;
				}
				ReadyRecordings.push_back(std::move(Task).TakeResult());
			}
			return true;
		};
		FRHIGPUSyncPointRef InitialSignal;
		if (!Context.InitialReleases.empty())
		{
			InitialSignal = CommandList.BeginGPUSubmission({.Queue = Context.Queues->Graphics});
			for (const auto& Transfer : Context.InitialReleases) CommandList.ReleaseQueueOwnership(Transfer);
			CommandList.EndGPUSubmission();
		}
		for (const auto& Batch : Compiled->ExecutionPlan.Batches)
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("RDG.RecordBatch");
			if (Context.bExplicitSubmissions)
			{
				FRHIGPUSubmissionDesc Desc{.Queue = Context.PhysicalQueue(Batch.Queue)};
				if (Context.WaitForInitial[Batch.Id.Index]) Desc.Waits.push_back(InitialSignal);
				for (uint32 Input : Context.Predecessors[Batch.Id.Index])
					Desc.Waits.push_back(State->SubmissionSyncPoints[Input]);
				State->SubmissionSyncPoints[Batch.Id.Index] = CommandList.BeginGPUSubmission(Desc);
			}
			struct FCloseSubmission
			{
				FRHICommandListImmediate& Commands;
				bool bEnabled;
				~FCloseSubmission() { if (bEnabled) Commands.EndGPUSubmission(); }
			} CloseSubmission{CommandList, Context.bExplicitSubmissions};
			for (const auto& Transfer : Context.Acquires[Batch.Id.Index]) CommandList.AcquireQueueOwnership(Transfer);
			if (Batch.bEpilogue)
			{
				RecordBarrierBatch(CommandList, Context.PreparedEpilogue, Context.PreparedTransitions);
				for (const auto& Transfer : Context.Releases[Batch.Id.Index]) CommandList.ReleaseQueueOwnership(Transfer);
				continue;
			}
			for (uint32 Index = Batch.FirstPass; Index < Batch.FirstPass + Batch.NumPasses; ++Index)
			{
				if (Compiled->RuntimePasses[Index].BufferUploadBytes != 0)
				{
					DURIN_PROFILE_CPU_ZONE_NAMED("RDG.RecordUploadBatch");
					uint64 Bytes = 0;
					uint32 End = Index;
					FRHICommandList Uploads;
					while (End < Batch.FirstPass + Batch.NumPasses && End - Index < MaxUploadBatchCount)
					{
						const auto& Runtime = Compiled->RuntimePasses[End];
						if (Runtime.BufferUploadBytes == 0 || Runtime.BufferUploadBytes > MaxUploadBatchBytes - Bytes) break;
						require(Runtime.RecordingPolicy == ERDGRecordingPolicy::Serial
							&& Runtime.RecordingExecute && *Runtime.RecordingExecute);
						const auto& Pass = Compiled->Passes[End];
						DURIN_PROFILE_CPU_ZONE_NAMED("RDG.RecordPass");
						DURIN_PROFILE_CPU_ZONE_TEXT(std::string_view(Pass.Name).substr(0, 128));
						// Keep barriers between uploads, including overlapping writes.
						RecordBarrierBatch(Uploads, Context.PreparedPassBarriers[End], Context.PreparedTransitions);
						const FRDGPassResources Resources(*this, End);
						const FRDGParameterResolver Resolver(Resources, Runtime.ParameterLayout,
							Runtime.OptionalAliases, Runtime.Parameters, Pass.Name, Pass.Type);
						(*Runtime.RecordingExecute)(Uploads, Resolver);
						Bytes += Runtime.BufferUploadBytes;
						++End;
					}
					require(End > Index);
					Uploads.FinishRecording();
					CommandList.QueueCommandList(std::move(Uploads));
					Index = End - 1;
					continue;
				}
				DURIN_PROFILE_CPU_ZONE_NAMED("RDG.RecordPass");
				const auto& Pass = Compiled->Passes[Index];
				DURIN_PROFILE_CPU_ZONE_TEXT(std::string_view(Pass.Name).substr(0, 128));
				const auto& Runtime = Compiled->RuntimePasses[Index];
				RecordBarrierBatch(CommandList, Context.PreparedPassBarriers[Index], Context.PreparedTransitions);
				if ((Runtime.ParameterizedExecute && *Runtime.ParameterizedExecute)
					|| (Runtime.RecordingExecute && *Runtime.RecordingExecute))
				{
					DURIN_PROFILE_CPU_ZONE_NAMED("RDG.PassCallback");
					DURIN_PROFILE_CPU_ZONE_TEXT(std::string_view(Pass.Name).substr(0, 128));
					const FRDGPassResources Resources(*this, Index);
					const FRDGParameterResolver Resolver(Resources,
						Runtime.ParameterLayout, Runtime.OptionalAliases,
						Runtime.Parameters,
						Pass.Name, Pass.Type);
					if (Runtime.RecordingExecute && *Runtime.RecordingExecute)
					{
						if (ReadyRecordings.empty() && !PrepareRecordingWave(Index))
							return std::unexpected(FRDGPreparationError{ERDGStateError::RecordingIncomplete});
						auto Recorded = std::move(ReadyRecordings.front());
						ReadyRecordings.pop_front();
						CommandList.QueueCommandList(std::move(Recorded));
					}
					else (*Runtime.ParameterizedExecute)(CommandList, Resolver);
				}
			}
			for (const auto& Transfer : Context.Releases[Batch.Id.Index]) CommandList.ReleaseQueueOwnership(Transfer);
		}
		return {};
	}

	auto FRDGBuilder::PublishExtractions() -> void
	{
		if (State->AllocationRetirement)
		{
			require(!State->SubmissionSyncPoints.empty());
			State->AllocationRetirement->Completion = State->SubmissionSyncPoints.back();
		}
		for (uint32 Index = 0; Index < Compiled->Resources.size(); ++Index)
		{
			const auto& Resource = Compiled->Resources[Index];
			const auto& Backing = Compiled->Backings[Index];
			if (Resource.TextureDestination != nullptr)
				*Resource.TextureDestination = Backing.Texture;
			if (Resource.BufferDestination != nullptr)
				*Resource.BufferDestination = Backing.Buffer;
		}
	}

	auto FRDGPassResources::GetTexture(
		FRDGTextureHandle Handle) const -> FRHITexture*
	{
		requiref(Handle.Owner == Graph.Compiled->Owner
			&& Handle.Index < Graph.Compiled->Resources.size(),
			"Render graph callback used an invalid texture handle.");
		requiref(PassIndex < Graph.Compiled->RuntimePasses.size()
			&& std::ranges::find(Graph.Compiled->RuntimePasses[PassIndex].ResourceIndices,
				Handle.Index) != Graph.Compiled->RuntimePasses[PassIndex].ResourceIndices.end(),
			"Render graph pass '{}' accessed undeclared texture resource {} ('{}').",
			PassIndex < Graph.Compiled->Passes.size()
				? Graph.Compiled->Passes[PassIndex].Name : "<invalid>", Handle.Index,
			Handle.Index < Graph.Compiled->Resources.size()
				? Graph.Compiled->Resources[Handle.Index].Name : "<invalid>");
		const auto& Resource = Graph.Compiled->Resources[Handle.Index];
		const auto& Backing = Graph.Compiled->Backings[Handle.Index];
		requiref(Resource.Kind == ERDGResourceKind::Texture && Backing.Texture, "Render graph callback resolved an unavailable texture.");
		return Backing.Texture.GetReference();
	}

	auto FRDGPassResources::GetBuffer(
		FRDGBufferHandle Handle) const -> FRHIBuffer*
	{
		requiref(Handle.Owner == Graph.Compiled->Owner
			&& Handle.Index < Graph.Compiled->Resources.size(),
			"Render graph callback used an invalid buffer handle.");
		requiref(PassIndex < Graph.Compiled->RuntimePasses.size()
			&& std::ranges::find(Graph.Compiled->RuntimePasses[PassIndex].ResourceIndices,
				Handle.Index) != Graph.Compiled->RuntimePasses[PassIndex].ResourceIndices.end(),
			"Render graph pass '{}' accessed undeclared buffer resource {} ('{}').",
			PassIndex < Graph.Compiled->Passes.size()
				? Graph.Compiled->Passes[PassIndex].Name : "<invalid>", Handle.Index,
			Handle.Index < Graph.Compiled->Resources.size()
				? Graph.Compiled->Resources[Handle.Index].Name : "<invalid>");
		const auto& Resource = Graph.Compiled->Resources[Handle.Index];
		const auto& Backing = Graph.Compiled->Backings[Handle.Index];
		requiref(Resource.Kind == ERDGResourceKind::Buffer && Backing.Buffer, "Render graph callback resolved an unavailable buffer.");
		return Backing.Buffer.GetReference();
	}

	auto FRDGPassResources::ResolveValue(uint64 Owner, uint32 Index,
		const void* TypeIdentity, bool bWrite) const -> void*
	{
		requiref(Owner == Graph.Compiled->Owner
			&& Index < Graph.Compiled->Resources.size(),
			"Render graph callback used an invalid typed value handle.");
		const auto& Resource = Graph.Compiled->Resources[Index];
		requiref(Resource.ValueTypeIdentity != nullptr
			&& Resource.ValueTypeIdentity == TypeIdentity,
			"Render graph callback used a wrongly typed value handle.");
		requiref(PassIndex < Graph.Compiled->RuntimePasses.size()
			&& std::ranges::any_of(Graph.Compiled->RuntimePasses[PassIndex].ValueUses,
				[&](const auto& Use) {
					return Use.first == Index && Use.second == (bWrite
						? ERDGUse::Write : ERDGUse::Read);
				}),
			"Render graph pass '{}' accessed typed value '{}' with an undeclared "
			"or wrong-direction capability.",
			PassIndex < Graph.Compiled->Passes.size()
				? Graph.Compiled->Passes[PassIndex].Name : "<invalid>", Resource.Name);
		requiref(Resource.ValueStorageIndex
			< Graph.State->ValueStorage.Allocations.size(),
			"Render graph callback resolved unavailable typed value storage.");
		return Graph.State->ValueStorage.Allocations[
			Resource.ValueStorageIndex]->Data;
	}

	auto FRDGAllocatedResources::SetTexture(uint32 ResourceId,
		FTextureRHIRef Texture, uint64 AllocationId,
		std::string_view Disposition) -> bool
	{
		if (ResourceId >= Textures.size() || !Texture) return false;
		Textures[ResourceId] = std::move(Texture);
		AllocationIds[ResourceId] = AllocationId;
		AllocationDispositions[ResourceId] = Disposition;
		return true;
	}

	auto FRDGAllocatedResources::SetBuffer(uint32 ResourceId,
		FBufferRHIRef Buffer, uint64 AllocationId,
		std::string_view Disposition) -> bool
	{
		if (ResourceId >= Buffers.size() || !Buffer) return false;
		Buffers[ResourceId] = std::move(Buffer);
		AllocationIds[ResourceId] = AllocationId;
		AllocationDispositions[ResourceId] = Disposition;
		return true;
	}

}
