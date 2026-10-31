#include "Texture/TextureCubeRenderResource.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	FTextureCubeResource::FTextureCubeResource(
		FTextureReference* InTextureReference,
		std::shared_ptr<const FTextureCubePlatformData> InPlatformData)
		: FTextureResource(InTextureReference)
		, PlatformData(std::move(InPlatformData))
	{
		check(PlatformData && PlatformData->IsValid());
	}

	FTextureCubeResource::~FTextureCubeResource() = default;

	auto FTextureCubeResource::InitRHI(FRHICommandListBase& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		// Commands copy upload bytes; the persistent render resource needs no CPU payload afterward.
		auto Input = std::move(PlatformData);
		check(Input != nullptr);

		const FTexture2DMipData& BaseMip =
			Input->Faces[0].Mips.front();
		FRHITextureCreateDesc Desc =
			FRHITextureCreateDesc::CreateCube("DTextureCube")
				.SetExtent(BaseMip.Width, BaseMip.Height)
				.SetFormat(Input->PixelFormat)
				.SetNumMips(static_cast<uint8>(
					Input->Faces[0].Mips.size()))
				.SetFlags(ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::CPUReadback);


		auto& CommandList =
			static_cast<FRHICommandListImmediate&>(RHICmdList);
		FTextureRHIRef NewTexture =
			GDynamicRHI->RHICreateTexture(CommandList, Desc);


		for (uint32 FaceIndex = 0;
			FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			for (uint32 MipIndex = 0;
				MipIndex < Input->Faces[FaceIndex].Mips.size();
				++MipIndex)
			{
				const FTexture2DMipData& Mip =
					Input->Faces[FaceIndex].Mips[MipIndex];
				const FUpdateTextureRegion2D Region(
					0, 0, 0, 0, Mip.Width, Mip.Height);
				CommandList.UpdateTexture2D(NewTexture, MipIndex, FaceIndex,
					Region, Mip.RowPitch, Mip.Pixels);
			}
		}

		const std::array Transitions{FRHITextureTransition::Whole(
			NewTexture, ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead)};
		CommandList.TransitionTextures(Transitions);
		SetTextureRHI_RenderThread(std::move(NewTexture));
	}

}
