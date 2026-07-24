#pragma once

#include "RHIResources.h"

namespace Durin
{
	struct FTexturePlatformData;
	struct FTextureSourceData;

	// Lightweight GPU texture owner for the Texture Editor preview panel.
	// Upload is synchronous (FlushRenderingCommands) because uploads are
	// bounded to one mip per user action and the user expects instant feedback.
	class FTexturePreview
	{
	public:
		FTexturePreview() = default;
		~FTexturePreview();

		FTexturePreview(const FTexturePreview&) = delete;
		FTexturePreview& operator=(const FTexturePreview&) = delete;

		// Upload the selected mip from platform data.
		auto Upload(const FTexturePlatformData& Platform, uint32 MipIndex) -> void;

		// Upload source RGBA8 data as a fallback preview (single mip).
		auto UploadSource(const FTextureSourceData& Source) -> void;

		// Release GPU resources and unregister from UI backend.
		auto Release() -> void;

		auto GetTexture() const -> FRHITexture* { return PreviewTexture.GetReference(); }
		auto GetWidth() const  -> uint32 { return PreviewWidth; }
		auto GetHeight() const -> uint32 { return PreviewHeight; }
		auto IsValid() const   -> bool { return PreviewTexture != nullptr; }

	private:
		auto UploadPixels(EPixelFormat Format, uint32 Width, uint32 Height, uint32 RowPitch, const uint8* Pixels) -> void;

		FTextureRHIRef PreviewTexture;
		uint32 PreviewWidth  = 0;
		uint32 PreviewHeight = 0;
	};
}
