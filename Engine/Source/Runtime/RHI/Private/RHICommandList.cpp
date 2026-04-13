#include "RHICommandList.h"

#include "DynamicRHI.h"
#include "RHIContext.h"

namespace Doge
{
	FRHICommandListExecutor GCommandListExecutor;

	FRHICommandList::FRHICommandList()
	{
	}

	auto FRHICommandList::SwitchPipeline(ERHIPipeline Pipeline) -> void
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

	auto FRHICommandList::BeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void
	{
		GetContext().RHIBeginRenderPass(Info, Name);
	}

	auto FRHICommandList::EndRenderPass() -> void
	{
		GetContext().RHIEndRenderPass();
	}

	auto FRHICommandList::BeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetTexture) -> void
	{
		GetContext().RHIBeginDrawingViewport(Viewport, RenderTargetTexture);
	}

	auto FRHICommandList::EndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
	{
		GetContext().RHIEndDrawingViewport(Viewport, bPresent, bLockToVsync);
	}

	auto FRHICommandList::SetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void
	{
		GetContext().RHISetGraphicsPipelineState(State);
	}

	auto FRHICommandList::BindVertexBuffer(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) -> void
	{
		GetContext().RHIBindVertexBuffer(StreamIndex, VertexBuffer, Offset);
	}

	auto FRHICommandList::DrawPrimitive() -> void
	{
		GetContext().RHIDrawPrimitive();
	}

	auto FRHICommandList::SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		GetContext().RHISetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
	}

	auto FRHICommandList::CreateBuffer(const FRHIBufferCreateDesc& InCreateDesc) -> TRefCountPtr<FRHIBuffer>
	{
		return nullptr;
	}

	auto FRHICommandList::LockBuffer(FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*
	{
		return GDynamicRHI->RHILockBuffer(*this, Buffer, Offset, Size, LockMode);
	}

	auto FRHICommandList::UnlockBuffer(FRHIBuffer* Buffer) -> void
	{
		return GDynamicRHI->RHIUnlockBuffer(*this, Buffer);
	}

	auto FRHICommandListImmediate::Get() -> FRHICommandListImmediate&
	{
		return GCommandListExecutor.GetImmediateCommandList();
	}

	auto FRHICommandListImmediate::ImmediateFlush(EImmediateFlushType FlushType, ERHISubmitFlags SubmitFlags/* = ERHISubmitFlags::None */) -> void
	{
		if (FlushType >= EImmediateFlushType::FlushRHIThread)
		{
			EnumAddFlags(SubmitFlags, ERHISubmitFlags::DeleteResources);
		}
		GCommandListExecutor.Submit({}, SubmitFlags);
	}

	FRHICommandListExecutor::FRHICommandListExecutor()
	{
	}

	auto FRHICommandListExecutor::GetImmediateCommandList() -> FRHICommandListImmediate&
	{
		return GCommandListExecutor.CommandListImmediate;
	}

	auto FRHICommandListExecutor::Submit(const std::vector<FRHICommandList*>& AdditionalCmdLists, ERHISubmitFlags SubmitFlags) -> void
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
} // namespace Doge