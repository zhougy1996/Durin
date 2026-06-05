#include "RHICommandList.h"

#include "DynamicRHI.h"
#include "RHIContext.h"

namespace Durin
{
	FRHICommandListExecutor GCommandListExecutor;

	FRHICommandListBase::FRHICommandListBase() = default;

	auto FRHICommandListBase::SwitchPipeline(ERHIPipeline Pipeline) -> void
	{
		if (ActivePipeline == Pipeline) return;

		ActivePipeline = Pipeline;

		switch (Pipeline)
		{
		case ERHIPipeline::Graphics:
			{
				GraphicsContext = GDynamicRHI->RHIGetDefaultContext();
			}
			break;
			// TODO: compute
		default:
			break;
		}
	}

	auto FRHICommandListBase::BeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void
	{
		GetContext().RHIBeginRenderPass(Info, Name);
	}

	auto FRHICommandListBase::EndRenderPass() -> void
	{
		GetContext().RHIEndRenderPass();
	}

	auto FRHICommandListBase::BeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetTexture) -> void
	{
		GetContext().RHIBeginDrawingViewport(Viewport, RenderTargetTexture);
	}

	auto FRHICommandListBase::EndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
	{
		GetContext().RHIEndDrawingViewport(Viewport, bPresent, bLockToVsync);
	}

	auto FRHICommandListBase::SetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void
	{
		GetContext().RHISetGraphicsPipelineState(State);
	}

	auto FRHICommandListBase::BindVertexBuffer(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) -> void
	{
		GetContext().RHIBindVertexBuffer(StreamIndex, VertexBuffer, Offset);
	}

	auto FRHICommandListBase::BindIndexBuffer(FRHIBuffer* Buffer, uint32 Offset) -> void
	{
		GetContext().RHIBindIndexBuffer(Buffer, Offset);
	}

	auto FRHICommandListBase::DrawIndexed(uint32 IndexCount, uint32 StartIndexLocation, int32 VertexOffset) -> void
	{
		GetContext().RHIDrawIndexed(IndexCount, StartIndexLocation, VertexOffset);
	}

	auto FRHICommandListBase::SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		GetContext().RHISetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
	}

	auto FRHICommandListBase::SetScissor(float MinX, float MinY, float Width, float Height) -> void
	{
		GetContext().RHISetScissor(MinX, MinY, Width, Height);
	}

	auto FRHICommandListBase::GetContext() const -> IRHICommandContext&
	{
		check(GraphicsContext && "No active pipeline or pipeline not supported yet.");
		return *GraphicsContext;
	}

	auto FRHICommandListBase::PushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void
	{
		GetContext().RHIPushConstants(StageFlags, Offset, Size, Data);
	}

	auto FRHICommandListBase::SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
	{
		GetContext().RHISetShaderParameters(InShader, InResourceParameters);
	}

	auto FRHICommandListImmediate::Get() -> FRHICommandListImmediate&
	{
		return GCommandListExecutor.GetImmediateCommandList();
	}

	auto FRHICommandListImmediate::ImmediateFlush(EImmediateFlushType FlushType, ERHISubmitFlags SubmitFlags /* = ERHISubmitFlags::None */) -> void
	{
		if (FlushType >= EImmediateFlushType::FlushRHIThread)
		{
			EnumAddFlags(SubmitFlags, ERHISubmitFlags::DeleteResources);
		}
		GCommandListExecutor.Submit({}, SubmitFlags);
	}

	auto FRHICommandListImmediate::LockBuffer(FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*
	{
		return GDynamicRHI->RHILockBuffer(*this, Buffer, Offset, Size, LockMode);
	}

	auto FRHICommandListImmediate::UnlockBuffer(FRHIBuffer* Buffer) -> void
	{
		return GDynamicRHI->RHIUnlockBuffer(*this, Buffer);
	}

	auto FRHICommandListImmediate::WriteBuffer(FRHIBuffer* Buffer, const void* Data, uint32 Size, uint32 OffsetBytes) -> void
	{
		void* MappedPointer = LockBuffer(Buffer, OffsetBytes, Size, EResourceLockMode::WriteOnly);
		std::memcpy(MappedPointer, Data, Size);
		UnlockBuffer(Buffer);
	}

	auto FRHICommandListImmediate::UpdateUniformBuffer(FRHIBuffer* UniformBuffer, const void* Data, uint32 Size, uint32 Offset) -> void
	{
		check(Size % 16 == 0 && Offset % 16 == 0);
		WriteBuffer(UniformBuffer, Data, Size, Offset);
	}

	FRHICommandListExecutor::FRHICommandListExecutor()
	{
	}

	auto FRHICommandListExecutor::GetImmediateCommandList() -> FRHICommandListImmediate&
	{
		return GCommandListExecutor.CommandListImmediate;
	}

	auto FRHICommandListExecutor::Submit(const std::vector<FRHICommandListBase*>& AdditionalCmdLists, ERHISubmitFlags SubmitFlags) -> void
	{
		if (EnumHasAnyFlags(SubmitFlags, ERHISubmitFlags::DeleteResources))
		{
			std::vector<FRHIResource*> ResourcesToDelete;
			while (true)
			{
				FRHIResource::GatherResourcesToDelete(ResourcesToDelete);
				if (!ResourcesToDelete.empty())
				{
					FRHIResource::DeleteResources(ResourcesToDelete);
					ResourcesToDelete.clear();
				}
				else
				{
					break;
				}
			}
		}

		if (EnumHasAnyFlags(SubmitFlags, ERHISubmitFlags::EndFrame))
		{
			GDynamicRHI->RHIEndFrame();
		}
	}
} // namespace Durin
