#include "Texture/TextureCubeRenderResource.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	FTextureCubeResource::FTextureCubeResource(
		FTextureReference* InTextureReference,
		std::shared_ptr<const FTextureCubePlatformData> InPlatformData,
		uint64 InRevision,
		std::shared_ptr<FTextureResourceCompletion> InCompletion)
		: FTextureAssetResource(
			InTextureReference, InRevision, std::move(InCompletion))
		, PlatformData(std::move(InPlatformData))
	{
		check(PlatformData && PlatformData->IsValid());
	}

	FTextureCubeResource::~FTextureCubeResource() = default;

	auto FTextureCubeResource::InitRHI(FRHICommandListBase& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		const uint64 Revision = GetRevision();
		const std::shared_ptr<FTextureResourceCompletion>& Completion =
			GetCompletion();
		if (!Completion->MarkBuilding(Revision)) return;

		const FTexture2DMipData& BaseMip =
			PlatformData->Faces[0].Mips.front();
		FRHITextureCreateDesc Desc =
			FRHITextureCreateDesc::CreateCube("DTextureCube")
				.SetExtent(BaseMip.Width, BaseMip.Height)
				.SetFormat(PlatformData->PixelFormat)
				.SetNumMips(static_cast<uint8>(
					PlatformData->Faces[0].Mips.size()))
				.SetFlags(ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::CPUReadback);
		if (!GDynamicRHI->RHIIsTextureFormatSupported(Desc))
		{
			Completion->MarkFailed(
				Revision, ETextureRenderFailure::UnsupportedFormat);
			return;
		}

		auto& CommandList =
			static_cast<FRHICommandListImmediate&>(RHICmdList);
		FTextureRHIRef NewTexture =
			GDynamicRHI->RHICreateTexture(CommandList, Desc);
		if (NewTexture == nullptr)
		{
			Completion->MarkFailed(
				Revision, ETextureRenderFailure::CreateOrUpload);
			return;
		}

		for (uint32 FaceIndex = 0;
			FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			for (uint32 MipIndex = 0;
				MipIndex < PlatformData->Faces[FaceIndex].Mips.size();
				++MipIndex)
			{
				const FTexture2DMipData& Mip =
					PlatformData->Faces[FaceIndex].Mips[MipIndex];
				const FUpdateTextureRegion2D Region(
					0, 0, 0, 0, Mip.Width, Mip.Height);
				GDynamicRHI->RHIUpdateTexture2D(
					CommandList, NewTexture, MipIndex, FaceIndex,
					Region, Mip.RowPitch, Mip.Pixels.data());
			}
		}

		if (Revision != Completion->GetRequestedRevision()) return;
		SetTextureRHI_RenderThread(std::move(NewTexture));
		PublishTexture_RenderThread();
		Completion->MarkReady(Revision);
	}

}
