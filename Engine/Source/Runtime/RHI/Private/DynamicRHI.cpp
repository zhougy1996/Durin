#include "DynamicRHI.h"

#include "RHICommandList.h"

namespace Durin
{
	const FRHIQueueCapabilities& FDynamicRHI::RHIGetQueueCapabilities() const
	{
		static const FRHIQueueCapabilities Unsupported;
		return Unsupported;
	}

	auto FDynamicRHI::RHIGetCompletionStatus(const FRHIGPUSubmissionTicket& Ticket) const
		-> ERHIGPUSubmissionState
	{
		const auto& Capabilities = RHIGetQueueCapabilities();
		const auto Point = Ticket.GetPoint();
		if (Point.DeviceGeneration == 0 || Point.DeviceGeneration != Capabilities.DeviceGeneration
			|| std::ranges::find(Capabilities.Queues, Point.Queue, &FRHIQueueInfo::Id) == Capabilities.Queues.end())
			return ERHIGPUSubmissionState::Invalid;
		return Ticket.GetState();
	}

	auto FDynamicRHI::RHIWaitForCompletion(const FRHIGPUSubmissionTicket&, uint64)
		-> ERHIGPUWaitResult
	{
		return ERHIGPUWaitResult::Invalid;
	}

	auto FDynamicRHI::GetPipelineCreationService() -> FRHIPipelineCreationService*
	{
		std::lock_guard Lock(PipelineCreationMutex);
		if (PipelineCreationClosed || !IsTaskSchedulerRunning() || !Capabilities) return nullptr;
		if (!PipelineCreation)
		{
			auto Backend = CreatePipelineCreationBackend();
			if (!Backend.FindGraphics || !Backend.FindCompute || !Backend.CreateGraphics
				|| !Backend.CreateCompute || !Backend.PublishTerminalFailure) return nullptr;
			PipelineCreation = std::make_unique<FRHIPipelineCreationService>(*Capabilities, std::move(Backend));
		}
		return PipelineCreation.get();
	}
	auto FDynamicRHI::RHIRequestGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer,
		std::string_view Name) -> FRHIPipelineCreationRequest
	{
		if (auto* Service = GetPipelineCreationService()) return Service->RequestGraphics(Initializer, Name);
		return FRHIPipelineCreationRequest::Rejected(RHIIsPipelineCreationClosed()
			? ERHIPipelineRequestRejection::Closed : ERHIPipelineRequestRejection::Unsupported);
	}
	auto FDynamicRHI::RHIRequestComputePipelineState(const FComputePipelineStateInitializer& Initializer,
		std::string_view Name) -> FRHIPipelineCreationRequest
	{
		if (auto* Service = GetPipelineCreationService()) return Service->RequestCompute(Initializer, Name);
		return FRHIPipelineCreationRequest::Rejected(RHIIsPipelineCreationClosed()
			? ERHIPipelineRequestRejection::Closed : ERHIPipelineRequestRejection::Unsupported);
	}
	auto FDynamicRHI::RHIRequestGraphicsPipelineBatch(std::span<const FRHIGraphicsPipelineBatchItem> Items)
		-> FRHIPipelineCreationBatch
	{
		if (auto* Service = GetPipelineCreationService()) return Service->RequestGraphicsBatch(Items);
		return {.Rejection = RHIIsPipelineCreationClosed() ? ERHIPipelineRequestRejection::Closed : ERHIPipelineRequestRejection::Unsupported};
	}
	auto FDynamicRHI::RHIRequestComputePipelineBatch(std::span<const FRHIComputePipelineBatchItem> Items)
		-> FRHIPipelineCreationBatch
	{
		if (auto* Service = GetPipelineCreationService()) return Service->RequestComputeBatch(Items);
		return {.Rejection = RHIIsPipelineCreationClosed() ? ERHIPipelineRequestRejection::Closed : ERHIPipelineRequestRejection::Unsupported};
	}
	auto FDynamicRHI::RHIStopPipelineCreation() -> void
	{
		FRHIPipelineCreationService* Service;
		{
			std::lock_guard Lock(PipelineCreationMutex);
			PipelineCreationClosed = true;
			Service = PipelineCreation.get();
		}
		if (Service) Service->CloseAndJoin(false);
	}
	auto FDynamicRHI::RHIRetirePipelineCreationResults() -> void
	{
		FRHIPipelineCreationService* Service;
		{
			std::lock_guard Lock(PipelineCreationMutex);
			Service = PipelineCreation.get();
		}
		if (Service) Service->CloseAndJoin();
	}
	auto FDynamicRHI::RHIIsPipelineCreationClosed() const -> bool
	{
		std::lock_guard Lock(PipelineCreationMutex);
		return PipelineCreationClosed || (PipelineCreation && PipelineCreation->IsClosed());
	}
	auto FDynamicRHI::RHIGetPipelineCreationStatistics() const -> FRHIPipelineCreationStatistics
	{
		std::lock_guard Lock(PipelineCreationMutex);
		return PipelineCreation ? PipelineCreation->GetStatistics() : FRHIPipelineCreationStatistics{};
	}

	auto FDynamicRHI::RHITryCreateTexture(FRHICommandListBase& RHICmdList,
		const FRHITextureCreateDesc& CreateDesc, ERHIResourceCreationFailure& OutFailure)
		-> TRefCountPtr<FRHITexture>
	{
		auto Resource = RHICreateTexture(RHICmdList, CreateDesc);
		OutFailure = Resource ? ERHIResourceCreationFailure::None
			: ERHIResourceCreationFailure::Unknown;
		return Resource;
	}

	auto FDynamicRHI::RHITryCreateBuffer(FRHICommandListImmediate& RHICmdList,
		const FRHIBufferCreateDesc& CreateDesc, ERHIResourceCreationFailure& OutFailure)
		-> TRefCountPtr<FRHIBuffer>
	{
		auto Resource = RHICreateBuffer(RHICmdList, CreateDesc);
		OutFailure = Resource ? ERHIResourceCreationFailure::None
			: ERHIResourceCreationFailure::Unknown;
		return Resource;
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
			S.Completion.RetirementPendingCount,
			S.Completion.RetirementHighWater,
			S.Completion.RetirementReleasedCount,
			S.Completion.RetirementMaxTokenLag,
			S.Messages.Total, S.Messages.Warning, S.Messages.Error,
			S.Naming.NamingAttempts, S.Naming.NamingFailures,
			S.Naming.LabelBegins, S.Naming.LabelEnds,
			S.Naming.ActiveRegionDepth, S.Timing.AllocatedPages,
			S.Timing.LiveIntervals, S.Timing.PendingIntervals,
			S.Timing.ReadyIntervals, S.Timing.ExhaustionCount,
			S.Timing.AllocationFailureCount);
	}

	auto FDynamicRHI::RHICreateGPUTimingQuery()
		-> TRefCountPtr<FRHIGPUTimingQuery>
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
		Result.Completion.RetirementPendingCount =
			Result.Memory.RetirementPendingCount;
		Result.Completion.RetirementHighWater =
			Result.Memory.RetirementHighWater;
		Result.Completion.RetirementReleasedCount =
			Result.Memory.RetirementReleasedCount;
		Result.Completion.RetirementMaxTokenLag =
			Result.Memory.RetirementMaxTokenLag;
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
		const FRHIBufferViewDesc& Desc) -> TRefCountPtr<FRHIBufferView>
	{
		std::string Error;
		if (!ValidateBufferViewDesc(Buffer, Desc, Error)) return nullptr;
		return new FRHIBufferView(Buffer, Desc);
	}

	auto FDynamicRHI::RHICreateTextureView(
		FRHITexture* Texture,
		const FRHITextureViewDesc& Desc) -> TRefCountPtr<FRHITextureView>
	{
		std::string Error;
		if (!ValidateTextureViewDesc(Texture, Desc, Error)) return nullptr;
		return new FRHITextureView(Texture, Desc);
	}

	auto FDynamicRHI::RHIGetOrCreateBufferView(
		FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc) -> TRefCountPtr<FRHIBufferView>
	{
		return RHICreateBufferView(Buffer, Desc);
	}

	auto FDynamicRHI::RHIGetOrCreateTextureView(
		FRHITexture* Texture,
		const FRHITextureViewDesc& Desc) -> TRefCountPtr<FRHITextureView>
	{
		return RHICreateTextureView(Texture, Desc);
	}

	FDynamicRHI* GDynamicRHI = nullptr;

	auto FDynamicRHI::RHIGetCapabilities() const -> const FRHICapabilities*
	{
		return Capabilities ? &*Capabilities : nullptr;
	}

	auto FDynamicRHI::RHICreateComputePipelineState(FName,
		const FComputePipelineStateInitializer&)
		-> TRefCountPtr<FRHIComputePipelineState>
	{
		return nullptr;
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
		RHICmdList.ImmediateFlush(
			EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::BeginFrame);
	}

	auto FDynamicRHI::RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void
	{
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread, ERHISubmitFlags::EndFrame | ERHISubmitFlags::DeleteResources);
	}

	auto FDynamicRHI::RHIAllocateDynamicUniformBuffer(
		FRHICommandListImmediate& RHICmdList,
		const void* Data,
		uint32 Size) -> FRHIUniformBufferRange
	{
		return RHICmdList.AllocateDynamicUniformBufferSynchronous(Data, Size);
	}

	auto FDynamicRHI::RHIAllocateDynamicStorageBuffer(
		FRHICommandListImmediate& RHICmdList,
		const void* Data,
		uint32 Size) -> FRHIStorageBufferRange
	{
		return RHICmdList.AllocateDynamicStorageBufferSynchronous(Data, Size);
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

	auto FDynamicRHI::RHIUpdateTexture2D(
		FRHICommandListBase& RHICmdList,
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		FByteView SourceData) -> void
	{
		RHICmdList.UpdateTexture2D(
			Texture, MipIndex, ArraySlice, UpdateRegion, SourcePitch, SourceData);
	}

	auto FDynamicRHI::RHIUpdateTexture3D(
		FRHICommandListBase& RHICmdList,
		FRHITexture* Texture,
		uint32 MipIndex,
		const FUpdateTextureRegion3D& UpdateRegion,
		uint32 SourceRowPitch,
		uint32 SourceDepthPitch,
		FByteView SourceData) -> void
	{
		RHICmdList.UpdateTexture3D(Texture, MipIndex, UpdateRegion,
			SourceRowPitch, SourceDepthPitch, SourceData);
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
