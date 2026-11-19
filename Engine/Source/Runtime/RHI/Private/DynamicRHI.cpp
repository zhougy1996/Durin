#include "Backend/RHICreation.h"
#include "DynamicRHI.h"
#include "Backend/RHIValidation.h"
#include "PipelineStateCache.h"
#include "Threading/TaskRuntime.h"
#include "Backend/RHICompletionBackend.h"

#include "RHICommandList.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	namespace
	{
		// A state can be requested by several passes. Keep only live identities,
		// so an editor session can discard material permutations after a reload.
		template<typename TState, typename TInitializer>
		auto CreateFixedState(const TInitializer& Initializer) -> std::shared_ptr<const TState>
		{
			static const auto Default = std::make_shared<const TState>(TInitializer{});
			if (Initializer == Default->GetInitializer()) return Default;
			static std::mutex Mutex;
			static std::vector<std::weak_ptr<const TState>> States;
			std::lock_guard Lock(Mutex);
			for (auto It = States.begin(); It != States.end();)
			{
				if (auto State = It->lock())
				{
					if (State->GetInitializer() == Initializer) return State;
					++It;
				}
				else It = States.erase(It);
			}
			auto State = std::make_shared<const TState>(Initializer);
			States.emplace_back(State);
			return State;
		}
	}

	auto FDynamicRHI::RHICreateBlendState(const FBlendStateInitializerRHI& Initializer) -> FBlendStateRHIRef
	{
		return CreateFixedState<FRHIBlendState>(Initializer);
	}

	auto RHICreateBlendState(const FBlendStateInitializerRHI& Initializer) -> FBlendStateRHIRef
	{
		if (GDynamicRHI) return GDynamicRHI->RHICreateBlendState(Initializer);
		return CreateFixedState<FRHIBlendState>(Initializer);
	}

	auto FDynamicRHI::RHICreateRasterizerState(const FRasterizerStateInitializerRHI& Initializer) -> FRasterizerStateRHIRef
	{
		return CreateFixedState<FRHIRasterizerState>(Initializer);
	}

	auto RHICreateRasterizerState(const FRasterizerStateInitializerRHI& Initializer) -> FRasterizerStateRHIRef
	{
		if (GDynamicRHI) return GDynamicRHI->RHICreateRasterizerState(Initializer);
		return CreateFixedState<FRHIRasterizerState>(Initializer);
	}

	auto FDynamicRHI::RHICreateDepthStencilState(const FDepthStencilStateInitializerRHI& Initializer) -> FDepthStencilStateRHIRef
	{
		return CreateFixedState<FRHIDepthStencilState>(Initializer);
	}

	auto RHICreateDepthStencilState(const FDepthStencilStateInitializerRHI& Initializer) -> FDepthStencilStateRHIRef
	{
		if (GDynamicRHI) return GDynamicRHI->RHICreateDepthStencilState(Initializer);
		return CreateFixedState<FRHIDepthStencilState>(Initializer);
	}

	FDynamicRHI::FDynamicRHI() = default;
	FDynamicRHI::~FDynamicRHI() = default;


	const FRHIQueueCapabilities& FDynamicRHI::RHIGetQueueCapabilities() const
	{
		static const FRHIQueueCapabilities Unsupported;
		return Unsupported;
	}

	auto FDynamicRHI::RHICreateAliasedTexture(const FRHITextureCreateDesc&, FRHITexture*) noexcept -> FTextureRHIRef
	{
		RHITerminateOnFailure("aliased texture creation", "transient aliasing unsupported");
	}
	auto FDynamicRHI::RHICreateAliasedBuffer(const FRHIBufferCreateDesc&, FRHIBuffer*) noexcept -> FBufferRHIRef
	{
		RHITerminateOnFailure("aliased buffer creation", "transient aliasing unsupported");
	}

	auto FDynamicRHI::RHICreateQueueTransfer(const FRHIQueueTransferDesc&) -> std::shared_ptr<FRHIQueueTransfer>
	{ return {}; }

	auto FDynamicRHI::RHICreateTransition(FRHITransitionDesc Desc)
		-> std::shared_ptr<FRHITransition>
	{
		if ((Desc.Buffers.empty() && Desc.Textures.empty())
			|| !ValidateBufferTransitions(Desc.Buffers)
			|| !ValidateTextureTransitions(Desc.Textures)) return {};
		return std::shared_ptr<FRHITransition>(new FRHITransition(std::move(Desc)));
	}

	auto FDynamicRHI::RHIGetCompletionStatus(const FRHIGPUSyncPointRef& SyncPoint) const
		-> ERHIGPUSubmissionState
	{
		const auto& Capabilities = RHIGetQueueCapabilities();
		const auto Point = FRHIGPUSyncPointBackend::GetPoint(SyncPoint);
		if (Point.DeviceGeneration == 0 || Point.DeviceGeneration != Capabilities.DeviceGeneration
			|| !std::ranges::contains(Capabilities.Queues, Point.Queue, &FRHIQueueInfo::Id))
			return ERHIGPUSubmissionState::Invalid;
		return SyncPoint.GetState();
	}

	auto FDynamicRHI::RHIWaitForCompletion(const FRHIGPUSyncPointRef&, uint64)
		-> ERHIGPUWaitResult
	{
		return ERHIGPUWaitResult::Invalid;
	}

	auto FDynamicRHI::RHIGetPipelineStateCache() -> FRHIPipelineStateCache*
	{
		return PipelineStateCache.get();
	}

	auto FDynamicRHI::InitializePipelineStateCache(const FRHICapabilities& DeviceCapabilities, FRHIPipelineCompileBackend Backend) -> void
	{
		check(!PipelineStateCache);
		// Hosts without Core workers use synchronous native pipeline creation.
		if (IsTaskSchedulerRunning())
			PipelineStateCache = std::make_unique<FRHIPipelineStateCache>(DeviceCapabilities, std::move(Backend));
	}

	auto FDynamicRHI::RHIGetPipelineCreationStatistics() const -> FRHIPipelineCreationStatistics
	{
		return PipelineStateCache ? PipelineStateCache->GetStatistics() : FRHIPipelineCreationStatistics{};
	}

	auto FDynamicRHI::RHICollectCompletedResources() -> void
	{
		GCommandListExecutor.ExecuteSynchronousOperation(false, [] {
			RHIFlushDeferredResources();
		});
	}

	auto FormatRHIDiagnosticSnapshot(
		const FRHIDiagnosticSnapshot& S) -> std::string
	{
		return std::format(
			"availability(requested={},debugUtils={},validation={},messenger={}) "
			"executor(commands={},pendingBatches={},pendingBytes={}) "
			"completion(submitted={},completed={},pending={},retirement={}/{}/{}/{}) "
			"messages(total={},warning={},error={}) "
			"naming(attempts={},failures={},labels={}/{},active={}) "
			"timing(pages={},live={},pending={},ready={},exhaustion={},failures={})",
			S.Availability.bRequested, S.Availability.bDebugUtilsActive,
			S.Availability.bValidationLayerActive, S.Availability.bMessengerActive,
			S.Executor.RecordedCommandCount, S.Executor.PendingBatchCount,
			S.Executor.PendingPayloadBytes, S.Completion.LastSubmittedToken,
			S.Completion.CompletedToken, S.Completion.PendingSubmissions,
			S.Memory.RetirementPendingCount,
			S.Memory.RetirementHighWater,
			S.Memory.RetirementReleasedCount,
			S.Memory.RetirementMaxTokenLag,
			S.Messages.Total, S.Messages.Warning, S.Messages.Error,
			S.Naming.NamingAttempts, S.Naming.NamingFailures,
			S.Naming.LabelBegins, S.Naming.LabelEnds,
			S.Naming.ActiveRegionDepth, S.Timing.AllocatedPages,
			S.Timing.LiveIntervals, S.Timing.PendingIntervals,
			S.Timing.ReadyIntervals, S.Timing.ExhaustionCount,
			S.Timing.AllocationFailureCount);
	}

	auto FDynamicRHI::RHICreateGPUTimingQuery() noexcept -> TRefCountPtr<FRHIGPUTimingQuery>
	{
		return nullptr;
	}

	auto FDynamicRHI::RHIGetGPUTimingResult(
		const FRHIGPUTimingQuery* Query) const -> FRHIGPUTimingResult
	{
		return Query ? Query->GetResult() : FRHIGPUTimingResult{};
	}

	auto FDynamicRHI::RHIGetDiagnosticSnapshot() const
		-> FRHIDiagnosticSnapshot
	{
		FRHIDiagnosticSnapshot Result;
		Result.Executor = GCommandListExecutor.GetStats();
		Result.PipelineCache = RHIGetPipelineCacheStatistics();
		Result.Memory = RHIGetMemoryStatistics();
		Result.Naming.InvalidRegionCount =
			FRHICommandListBase::GetInvalidDiagnosticRegionCount();
		return Result;
	}

	auto FDynamicRHI::RHIResetDiagnosticStatistics() -> void
	{
		RHIResetPipelineCacheStatistics();
		RHIResetMemoryStatistics();
		FRHICommandListBase::ResetInvalidDiagnosticRegionCount();
	}

	auto FDynamicRHI::RHICreateBufferView(
		FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc
	) noexcept -> TRefCountPtr<FRHIBufferView>
	{
		if (!ValidateBufferViewDesc(Buffer, Desc)) RHITerminateOnFailure("native resource creation", "invalid or unsupported creation request");
		return new FRHIBufferView(Buffer, Desc);
	}

	auto FDynamicRHI::RHICreateTextureView(
		FRHITexture* Texture,
		const FRHITextureViewDesc& Desc
	) noexcept -> TRefCountPtr<FRHITextureView>
	{
		if (!ValidateTextureViewDesc(Texture, Desc)) RHITerminateOnFailure("native resource creation", "invalid or unsupported creation request");
		return new FRHITextureView(Texture, Desc);
	}

	auto FRHITextureViewCache::GetOrCreate(FRHITexture* Resource, const FRHITextureViewDesc& Desc) -> FTextureViewRHIRef
	{
		std::lock_guard Lock(Mutex);
		for (const auto& View : Views)
		{
			if (View->GetTexture() == Resource && View->GetDesc() == Desc) return View;
		}
		FTextureViewRHIRef View = GDynamicRHI ? GDynamicRHI->RHICreateTextureView(Resource, Desc)
			: FTextureViewRHIRef(new FRHITextureView(Resource, Desc));
		Views.push_back(View);
		return View;
	}

	FDynamicRHI* GDynamicRHI = nullptr;

	auto FDynamicRHI::RHIGetCapabilities() const -> const FRHICapabilities*
	{
		return Capabilities ? &*Capabilities : nullptr;
	}

	auto FDynamicRHI::RHICreateComputePipelineState(FName, const FComputePipelineStateInitializer&) noexcept -> TRefCountPtr<FRHIComputePipelineState>
	{
		RHITerminateOnFailure("native resource creation", "invalid or unsupported creation request");
	}

	auto FDynamicRHI::RHIGetPipelineCacheStatistics() const
		-> FRHIPipelineCacheStatistics
	{
		return {};
	}

	auto FDynamicRHI::RHIResetPipelineCacheStatistics() -> void
	{
	}

	auto FDynamicRHI::RHIGetMemoryStatistics() const -> FRHIMemoryStatistics
	{
		return {};
	}

	auto FDynamicRHI::RHIResetMemoryStatistics() -> void
	{
	}

	auto FDynamicRHI::PublishCapabilities(FRHICapabilities InCapabilities) -> void
	{
		check(!Capabilities.has_value());
		check(InCapabilities.SupportedTextureDimensions != ERHITextureDimensionFlags::None);
		check(InCapabilities.MaxTextureDimension2D > 0);
		check(InCapabilities.MaxTextureDimensionCube > 0);
		check(InCapabilities.MaxTextureArrayLayers >= TextureCubeFaceCount);
		check(InCapabilities.ColorSampleCounts != ERHISampleCountFlags::None);
		check(InCapabilities.DepthSampleCounts != ERHISampleCountFlags::None);
		check(std::ranges::all_of(InCapabilities.MaxComputeWorkGroupCount,
			[](uint32 Limit) { return Limit > 0; }));
		Capabilities.emplace(std::move(InCapabilities));
	}

	auto FDynamicRHI::ClearCapabilities() -> void
	{
		Capabilities.reset();
	}

	auto FDynamicRHI::RHIUpdateTextureReference(
		FRHITextureReference* TextureReference,
		FRHITexture* NewTexture) -> void
	{
		check(TextureReference != nullptr);
		TextureReference->SetReferencedTexture_RenderThread(NewTexture);
	}

	auto FDynamicRHI::RHIBeginFrame_RenderThread(
		FRHICommandListImmediate& RHICmdList) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RHI.BeginFrame.Dispatch");
		RHICmdList.ImmediateFlush(
			EImmediateFlushType::DispatchToRHIThread,
			ERHISubmitFlags::BeginFrame);
	}

	auto FDynamicRHI::RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void
	{
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread, ERHISubmitFlags::EndFrame | ERHISubmitFlags::DeleteResources);
	}

	auto FDynamicRHI::RHILockBuffer(
		FRHICommandListImmediate& RHICmdList,
		FRHIBuffer* Buffer,
		uint32 Offset,
		uint32 Size,
		EResourceLockMode LockMode) -> void*
	{
		return RHICmdList.LockBuffer(Buffer, Offset, Size, LockMode);
	}

	auto FDynamicRHI::RHIUnlockBuffer(
		FRHICommandListImmediate& RHICmdList,
		FRHIBuffer* Buffer) -> void
	{
		RHICmdList.UnlockBuffer(Buffer);
	}

	auto FDynamicRHI::RHIReadTexture2D(
		FRHICommandListImmediate& RHICmdList,
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		FByteBuffer& OutData) -> bool
	{
		return RHICmdList.ReadTexture2D(
			Texture, MipIndex, ArraySlice, OutData);
	}

	auto FDynamicRHI::RHIBlockUntilGPUIdle() -> void
	{
		FRHICommandListImmediate::Get().BlockUntilGPUIdle();
	}
} // namespace Durin
