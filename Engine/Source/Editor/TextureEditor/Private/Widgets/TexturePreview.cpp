#include "Widgets/TexturePreview.h"

#include "DynamicRHI.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	FTexturePreview::~FTexturePreview()
	{
		Release();
	}

	auto FTexturePreview::UploadPixels(EPixelFormat Format, uint32 Width, uint32 Height, uint32 RowPitch, const uint8* Pixels) -> void
	{
		Release();

		if (!GDynamicRHI || !Pixels || Width == 0 || Height == 0) return;

		// Capture a snapshot of the pixel data for the render command.
		const size_t PixelCount = static_cast<size_t>(RowPitch) * Height;
		auto PixelSnapshot = std::make_shared<std::vector<uint8>>(Pixels, Pixels + PixelCount);

		FTextureRHIRef NewTexture;
		ENQUEUE_RENDER_COMMAND(UploadTexturePreview)([&NewTexture, Format, Width, Height, RowPitch, PixelSnapshot](FRHICommandListImmediate& CommandList) {
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("TexturePreview", Width, Height, Format);
			Desc.AddFlags(ETextureCreateFlags::ShaderResource);
			NewTexture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (NewTexture)
			{
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, Width, Height);
				GDynamicRHI->RHIUpdateTexture2D(CommandList, NewTexture, 0, 0, Region, RowPitch, PixelSnapshot->data());
			}
		});
		FlushRenderingCommands();

		if (!NewTexture || !Mona::GActiveUIBackend) return;

		Mona::GActiveUIBackend->RegisterTexture(NewTexture);
		PreviewTexture = std::move(NewTexture);
		PreviewWidth = Width;
		PreviewHeight = Height;
	}

	auto FTexturePreview::Upload(const FTexturePlatformData& Platform, uint32 MipIndex) -> void
	{
		if (!Platform.IsValid() || MipIndex >= Platform.Mips.size()) return;
		const FTexture2DMipData& Mip = Platform.Mips[MipIndex];
		UploadPixels(Platform.PixelFormat, Mip.Width, Mip.Height, Mip.RowPitch, Mip.Pixels.data());
	}

	auto FTexturePreview::UploadSource(const FTextureSourceData& Source) -> void
	{
		if (!Source.IsValid()) return;
		// Source data is always RGBA8; preview it without color-space conversion.
		UploadPixels(EPixelFormat::RGBA8_UNORM, Source.Width, Source.Height, Source.Width * 4, Source.Pixels.data());
	}

	auto FTexturePreview::Release() -> void
	{
		if (PreviewTexture && Mona::GActiveUIBackend)
			Mona::GActiveUIBackend->UnregisterTexture(PreviewTexture);
		PreviewTexture = nullptr;
		PreviewWidth = 0;
		PreviewHeight = 0;
	}
}
