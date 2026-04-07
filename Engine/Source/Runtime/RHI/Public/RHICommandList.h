#pragma once

#include "DynamicRHI.h"
#include "RHIDefinitions.h"
#include "RHIResources.h"

namespace Doge
{
	struct FRHITextureCreateDesc;
	struct FRHIRenderPassInfo;

	class FRHITexture;
	class IRHICommandContext;
	class FRHICommandListExecutor;
	class FRHIViewport;
	class FRHIGraphicsPipelineState;

	class RHI_API FRHICommandList
	{
	public:
		FRHICommandList();

		virtual ~FRHICommandList() = default;

		// Call this function to switch between graphics and compute pipelines
		// This function will set the context, so call this before any other command
		auto SwitchPipeline(ERHIPipeline Pipeline) -> void;

		auto BeginFrame() -> void;

		auto EndFrame() -> void;

		auto BeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void;

		auto EndRenderPass() -> void;

		auto BeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetTexture) -> void;

		auto EndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void;

		auto SetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void;

		auto DrawPrimitive() -> void;

		auto SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;

		auto GetContext() const -> IRHICommandContext& { return *GraphicsContext; }

		auto SubmitCommandsHint() -> void;

		auto CreateBuffer(const FRHIBufferCreateDesc& InCreateDesc) -> TRefCountPtr<FRHIBuffer>;

		auto LockBuffer(FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*;

		auto UnlockBuffer(FRHIBuffer* Buffer) -> void;

	private:
		ERHIPipeline ActivePipeline = ERHIPipeline::None;

		IRHICommandContext* GraphicsContext = nullptr;
	};

	enum class EImmediateFlushType
	{
		WaitForOutstandingTasksOnly,
		DispatchToRHIThread,
		FlushRHIThread,
		FlushRHIThreadFlushResources
	};

	enum class ERHISubmitFlags
	{
		None = 0,
		SubmitToGPU = 1 << 0,
		DeleteResources = 1 << 1,
		FlushRHIThread = 1 << 2,

		// Mark the end of a engine frame
		EndFrame = 1 << 3,
	};

	ENUM_CLASS_FLAGS(ERHISubmitFlags)

	// Main command list class that will be used to record commands, and submit to GPU immediately when calling EndFrame
	class RHI_API FRHICommandListImmediate : public FRHICommandList
	{
	public:
		auto SubmitAndBlockUntilGPUIdle() -> void;

		static auto Get() -> FRHICommandListImmediate&;

		auto ImmediateFlush(EImmediateFlushType FlushType, ERHISubmitFlags SubmitFlags = ERHISubmitFlags::None) -> void;
	};

	class RHI_API FRHICommandListExecutor
	{
	public:
		FRHICommandListExecutor();

		auto GetImmediateCommandList() -> FRHICommandListImmediate&;

		auto Submit(const std::vector<FRHICommandList*>& AdditionalCmdLists, ERHISubmitFlags SubmitFlags) -> void;

	private:
		FRHICommandListImmediate CommandListImmediate;
	};

	extern RHI_API FRHICommandListExecutor GCommandListExecutor;

	FORCEINLINE TRefCountPtr<FRHITexture> RHICreateTexture(const FRHITextureCreateDesc& CreateDesc)
	{
		return GDynamicRHI->RHICreateTexture(FRHICommandListImmediate::Get(), CreateDesc);
	}

	FORCEINLINE TRefCountPtr<FRHIBuffer> RHICreateBuffer(const FRHIBufferCreateDesc& CreateDesc)
	{
		return GDynamicRHI->RHICreateBuffer(FRHICommandListImmediate::Get(), CreateDesc);
	}
} // namespace Doge