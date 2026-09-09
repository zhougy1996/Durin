#include "Texture/VolumeTextureRenderResource.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	FVolumeTextureResource::FVolumeTextureResource(
		FTextureReference* InTextureReference,
		std::shared_ptr<const FVolumeTexturePlatformData> InPlatformData)
		: FTextureResource(InTextureReference)
		, PlatformData(std::move(InPlatformData))
	{
		check(PlatformData && PlatformData->IsValid());
	}

	FVolumeTextureResource::~FVolumeTextureResource() = default;

	auto FVolumeTextureResource::InitRHI(FRHICommandListBase& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		const FVolumeTextureMipData& BaseMip = PlatformData->Mips.front();
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D("DVolumeTexture")
			.SetExtent(BaseMip.Width, BaseMip.Height)
			.SetDepth(static_cast<uint16>(BaseMip.Depth))
			.SetFormat(PlatformData->PixelFormat)
			.SetNumMips(static_cast<uint8>(PlatformData->Mips.size()))
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
		if (!GDynamicRHI->RHIIsTextureSupported(Desc))
		{
			DURIN_WARN("VolumeTexture description is unsupported by the RHI (format: {}).", static_cast<uint32>(Desc.Format));
			return;
		}
		auto& CommandList = static_cast<FRHICommandListImmediate&>(RHICmdList);
		FTextureRHIRef NewTexture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
		if (!NewTexture)
		{
			DURIN_WARN("VolumeTexture GPU texture allocation failed.");
			return;
		}
		for (uint32 MipIndex = 0; MipIndex < PlatformData->Mips.size(); ++MipIndex)
		{
			const FVolumeTextureMipData& Mip = PlatformData->Mips[MipIndex];
			const FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0,
				Mip.Width, Mip.Height, Mip.Depth);
			GDynamicRHI->RHIUpdateTexture3D(CommandList, NewTexture, MipIndex,
				Region, Mip.RowPitch, Mip.DepthPitch, Mip.Voxels);
		}
		SetTextureRHI_RenderThread(std::move(NewTexture));
	}
}
