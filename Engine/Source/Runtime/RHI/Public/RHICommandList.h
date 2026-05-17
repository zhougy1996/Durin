#pragma once

#include "RHIAPI.h"
#include "DynamicRHI.h"
#include "RHIDefinitions.h"
#include "RHIResources.h"
#include "RHIShaderParameters.h"

namespace Durin
{
	struct FRHITextureCreateDesc;
	struct FRHIRenderPassInfo;

	class FRHITexture;
	class IRHICommandContext;
	class FRHICommandListExecutor;
	class FRHIViewport;
	class FRHIGraphicsPipelineState;

	class FRHICommandListBase
	{
	public:
		RHI_API FRHICommandListBase();

		virtual ~FRHICommandListBase() = default;

		// Call this function to switch between graphics and compute pipelines
		// This function will set the context, so call this before any other command
		RHI_API auto SwitchPipeline(ERHIPipeline Pipeline) -> void;

		RHI_API auto BeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void;

		RHI_API auto EndRenderPass() -> void;

		RHI_API auto BeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetTexture) -> void;

		RHI_API auto EndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void;

		RHI_API auto SetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void;

		RHI_API auto BindVertexBuffer(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) -> void;

		RHI_API auto BindIndexBuffer(FRHIBuffer* Buffer, uint32 Offset) -> void;

		RHI_API auto DrawIndexed(uint32 IndexCount, uint32 StartIndexLocation, int32 VertexOffset) -> void;

		RHI_API auto SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;

		RHI_API auto GetContext() const -> IRHICommandContext&;

		RHI_API auto PushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void;

		RHI_API auto SetShaderParameters(FRHIShader* InShader, std::span<uint8> InParametersData) -> void;

		RHI_API auto SetShaderParameters(FRHIShader* InShader, std::span<FRHIShaderParameterResource> InResourceParameters) -> void;

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

	class FRHICommandListImmediate : public FRHICommandListBase
	{
	public:
		RHI_API static auto Get() -> FRHICommandListImmediate&;

		RHI_API auto ImmediateFlush(EImmediateFlushType FlushType, ERHISubmitFlags SubmitFlags = ERHISubmitFlags::None) -> void;

		RHI_API auto LockBuffer(FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*;

		RHI_API auto UnlockBuffer(FRHIBuffer* Buffer) -> void;

		RHI_API auto WriteBuffer(FRHIBuffer* Buffer, const void* Data, uint32 Size, uint32 OffsetBytes) -> void;

		RHI_API auto UpdateUniformBuffer(FRHIBuffer* UniformBuffer, const void* Data, uint32 Size, uint32 Offset) -> void;
	};

	class FRHICommandListExecutor
	{
	public:
		RHI_API FRHICommandListExecutor();

		RHI_API auto GetImmediateCommandList() -> FRHICommandListImmediate&;

		RHI_API auto Submit(const std::vector<FRHICommandListBase*>& AdditionalCmdLists, ERHISubmitFlags SubmitFlags) -> void;

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