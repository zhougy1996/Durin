#include "Texture/Texture2DRenderResource.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	FTexture2DResource::FTexture2DResource(
		FTextureReference* InTextureReference,
		std::shared_ptr<const FTexturePlatformData> InPlatformData)
		: FTextureResource(InTextureReference)
		, PlatformData(std::move(InPlatformData))
	{
		check(PlatformData && PlatformData->IsValid());
	}

	FTexture2DResource::~FTexture2DResource() = default;

	auto FTexture2DResource::InitRHI(FRHICommandListBase& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		// Commands copy upload bytes; the persistent render resource needs no CPU payload afterward.
		auto Input = std::move(PlatformData);
		check(Input != nullptr);

		const FTexture2DMipData& BaseMip = Input->Mips.front();
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
			"DTexture2D", BaseMip.Width, BaseMip.Height,
			Input->PixelFormat)
			.SetNumMips(static_cast<uint8>(Input->Mips.size()))
			.SetFlags(ETextureCreateFlags::ShaderResource);


		auto& CommandList =
			static_cast<FRHICommandListImmediate&>(RHICmdList);
		FTextureRHIRef NewTexture =
			GDynamicRHI->RHICreateTexture(CommandList, Desc);


		for (uint32 MipIndex = 0;
			MipIndex < Input->Mips.size(); ++MipIndex)
		{
			const FTexture2DMipData& Mip = Input->Mips[MipIndex];
			const FUpdateTextureRegion2D Region(
				0, 0, 0, 0, Mip.Width, Mip.Height);
			CommandList.UpdateTexture2D(NewTexture, MipIndex, 0, Region,
				Mip.RowPitch, Mip.Pixels);
		}

		const std::array Transitions{FRHITextureTransition::Whole(
			NewTexture, ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead)};
		CommandList.TransitionTextures(Transitions);
		SetTextureRHI_RenderThread(std::move(NewTexture));
	}

}
