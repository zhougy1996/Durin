#include "RDGBuilderInternal.h"
#include "Misc/Time.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"

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
		auto RecordBarrierBatch(FRHICommandListImmediate& CommandList,
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

		auto RecordBarrierBatch(FRHICommandListImmediate& CommandList,
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

	auto FRDGBuilder::Execute(FRHICommandListImmediate& CommandList,
		FRDGAllocator* Allocator) -> FRDGExecutionResult
	{
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
		FScopedMicrosecondTimer ExecuteTimer(State->ExecuteMicroseconds);
		FScopedMicrosecondTimer PreparationTimer(State->Phases.PreparationMicroseconds);
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
			auto AllocationResult = Allocator->Allocate(Requests, Candidate);
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
		FPreparedTransitions PreparedTransitions;
		std::vector<FPreparedBarrierBatch> PreparedPassBarriers;
		PreparedPassBarriers.reserve(Compiled->Passes.size());
		for (const auto& Pass : Compiled->Passes)
			PreparedPassBarriers.push_back(PrepareBarrierBatch(Pass.Barriers, Compiled->Backings, PreparedTransitions));
		const auto PreparedEpilogue = PrepareBarrierBatch(Compiled->FinalBarriers, Compiled->Backings, PreparedTransitions);
		const auto* Queues = GDynamicRHI ? &GDynamicRHI->RHIGetQueueCapabilities() : nullptr;
		const bool bExplicitSubmissions = Queues && !Queues->Queues.empty();
		const bool bAsync = State->bAsyncComputeEnabled && bExplicitSubmissions
			&& Queues->bIndependentCompute && Queues->Compute != Queues->Graphics
			&& (Compiled->AllocationRequests.empty() || (Allocator && Allocator->SupportsAsyncCompute()));
		auto PhysicalQueue = [&](ERDGQueueAssignment Queue) {
			return bAsync && Queue == ERDGQueueAssignment::AsyncCompute ? Queues->Compute : Queues->Graphics;
		};
		using FTransfers = std::vector<std::shared_ptr<FRHIQueueTransfer>>;
		std::vector<FTransfers> Acquires(Compiled->ExecutionPlan.Batches.size()), Releases(Acquires.size());
		FTransfers InitialReleases;
		std::vector<bool> WaitForInitial(Acquires.size(), false);
		if (bAsync)
		{
			PreparedTransitions.TransferredBuffers.resize(PreparedTransitions.Buffers.size(), false);
			PreparedTransitions.TransferredTextures.resize(PreparedTransitions.Textures.size(), false);
			for (const auto& Handoff : Compiled->ExecutionPlan.Handoffs)
			{
				const auto& Consumer = Compiled->ExecutionPlan.Batches[Handoff.Consumer.Index];
				if (Handoff.SourceQueue == Consumer.Queue) continue;
				FRHIQueueTransferDesc Desc{.Source = PhysicalQueue(Handoff.SourceQueue), .Destination = PhysicalQueue(Consumer.Queue)};
				const auto& Barrier = Consumer.bEpilogue ? PreparedEpilogue : PreparedPassBarriers[Consumer.FirstPass];
				if (Handoff.bTexture)
				{
					const size_t Index = Barrier.FirstTexture + Handoff.TransitionIndex;
					Desc.Textures.push_back(PreparedTransitions.Textures[Index]);
					PreparedTransitions.TransferredTextures[Index] = true;
				}
				else
				{
					const size_t Index = Barrier.FirstBuffer + Handoff.TransitionIndex;
					Desc.Buffers.push_back(PreparedTransitions.Buffers[Index]);
					PreparedTransitions.TransferredBuffers[Index] = true;
				}
				auto Transfer = GDynamicRHI->RHICreateQueueTransfer(Desc);
				if (!Transfer) return std::unexpected(ERDGPreparationError::QueueTransferFailed);
				Acquires[Consumer.Id.Index].push_back(Transfer);
				const auto Producer = std::ranges::find_if(Handoff.Producers, [&](const auto Id) {
					return Compiled->ExecutionPlan.Batches[Id.Index].Queue == Handoff.SourceQueue;
				});
				if (Producer != Handoff.Producers.end()) Releases[Producer->Index].push_back(std::move(Transfer));
				else
				{
					require(Handoff.SourceQueue == ERDGQueueAssignment::Graphics);
					InitialReleases.push_back(std::move(Transfer));
					WaitForInitial[Consumer.Id.Index] = true;
				}
			}
		}
		std::vector<std::vector<uint32>> Predecessors(Compiled->ExecutionPlan.Batches.size());
		if (bExplicitSubmissions)
		{
			State->SubmissionSyncPoints.resize(Compiled->ExecutionPlan.Batches.size());
			for (const auto& Edge : Compiled->ExecutionPlan.Dependencies)
				Predecessors[Edge.After.Index].push_back(Edge.Before.Index);
			for (auto& Inputs : Predecessors)
			{
				std::ranges::sort(Inputs);
				Inputs.erase(std::unique(Inputs.begin(), Inputs.end()), Inputs.end());
			}
		}
		PreparationTimer.Stop();
		FScopedMicrosecondTimer RecordingTimer(State->Phases.RecordingMicroseconds);
		State->Lifecycle = ERDGBuilderState::Recording;
		State->ExecutionResult = std::unexpected(FRDGExecutionError{ERDGStateError::RecordingIncomplete});
		FRHIGPUSyncPointRef InitialSignal;
		if (!InitialReleases.empty())
		{
			InitialSignal = CommandList.BeginGPUSubmission({.Queue = Queues->Graphics});
			for (const auto& Transfer : InitialReleases) CommandList.ReleaseQueueOwnership(Transfer);
			CommandList.EndGPUSubmission();
		}
		for (const auto& Batch : Compiled->ExecutionPlan.Batches)
		{
			if (bExplicitSubmissions)
			{
				FRHIGPUSubmissionDesc Desc{.Queue = PhysicalQueue(Batch.Queue)};
				if (WaitForInitial[Batch.Id.Index]) Desc.Waits.push_back(InitialSignal);
				for (uint32 Input : Predecessors[Batch.Id.Index])
					Desc.Waits.push_back(State->SubmissionSyncPoints[Input]);
				State->SubmissionSyncPoints[Batch.Id.Index] = CommandList.BeginGPUSubmission(Desc);
			}
			struct FCloseSubmission
			{
				FRHICommandListImmediate& Commands;
				bool bEnabled;
				~FCloseSubmission() { if (bEnabled) Commands.EndGPUSubmission(); }
			} CloseSubmission{CommandList, bExplicitSubmissions};
			for (const auto& Transfer : Acquires[Batch.Id.Index]) CommandList.AcquireQueueOwnership(Transfer);
			if (Batch.bEpilogue)
			{
				RecordBarrierBatch(CommandList, PreparedEpilogue, PreparedTransitions);
				for (const auto& Transfer : Releases[Batch.Id.Index]) CommandList.ReleaseQueueOwnership(Transfer);
				continue;
			}
			for (uint32 Index = Batch.FirstPass; Index < Batch.FirstPass + Batch.NumPasses; ++Index)
			{
				const auto& Pass = Compiled->Passes[Index];
				const auto& Runtime = Compiled->RuntimePasses[Index];
				RecordBarrierBatch(CommandList, PreparedPassBarriers[Index], PreparedTransitions);
				if (Runtime.ParameterizedExecute != nullptr && *Runtime.ParameterizedExecute)
				{
					const FRDGPassResources Resources(*this, Index);
					const FRDGParameterResolver Resolver(Resources,
						Runtime.ParameterLayout, Runtime.OptionalAliases,
						Runtime.Parameters,
						Pass.Name, Pass.Type);
					(*Runtime.ParameterizedExecute)(CommandList, Resolver);
				}
			}
			for (const auto& Transfer : Releases[Batch.Id.Index]) CommandList.ReleaseQueueOwnership(Transfer);
		}
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
		return {};
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
