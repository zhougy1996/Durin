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

		const FTexture2DMipData& BaseMip = PlatformData->Mips.front();
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
			"DTexture2D", BaseMip.Width, BaseMip.Height,
			PlatformData->PixelFormat)
			.SetNumMips(static_cast<uint8>(PlatformData->Mips.size()))
			.SetFlags(ETextureCreateFlags::ShaderResource);
		if (!GDynamicRHI->RHIIsTextureSupported(Desc))
		{
			DURIN_WARN("Texture2D description is unsupported by the RHI (format: {}).", static_cast<uint32>(Desc.Format));
			return;
		}

		auto& CommandList =
			static_cast<FRHICommandListImmediate&>(RHICmdList);
		FTextureRHIRef NewTexture =
			GDynamicRHI->RHICreateTexture(CommandList, Desc);
		if (NewTexture == nullptr)
		{
			DURIN_WARN("Texture2D GPU texture allocation failed.");
			return;
		}

		for (uint32 MipIndex = 0;
			MipIndex < PlatformData->Mips.size(); ++MipIndex)
		{
			const FTexture2DMipData& Mip = PlatformData->Mips[MipIndex];
			const FUpdateTextureRegion2D Region(
				0, 0, 0, 0, Mip.Width, Mip.Height);
			GDynamicRHI->RHIUpdateTexture2D(
				CommandList, NewTexture, MipIndex, 0, Region,
				Mip.RowPitch, Mip.Pixels);
		}

		SetTextureRHI_RenderThread(std::move(NewTexture));
	}

}
