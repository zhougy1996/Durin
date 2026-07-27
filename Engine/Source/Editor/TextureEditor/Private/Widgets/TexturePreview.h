#pragma once

#include "RHIResources.h"

namespace Durin
{
	struct FTexturePlatformData;
	struct FTextureSourceData;

	enum class ETexturePreviewChannel : uint8
	{
		RGBA,
		Red,
		Green,
		Blue,
		Alpha,
	};

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
		auto Upload(
			const FTexturePlatformData& Platform,
			uint32 MipIndex,
			ETexturePreviewChannel Channel = ETexturePreviewChannel::RGBA
		) -> void;

		// Upload source RGBA8 data as a fallback preview (single mip).
		auto UploadSource(
			const FTextureSourceData& Source,
			ETexturePreviewChannel Channel = ETexturePreviewChannel::RGBA
		) -> void;

		// Select the displayed channel without re-uploading the source texture.
		auto SetChannel(ETexturePreviewChannel Channel) -> void;

		// Release GPU resources and unregister from UI backend.
		auto Release() -> void;

		// Release the shared channel-filter pipeline before the render thread stops.
		static auto ReleaseSharedResources() -> void;

		auto GetTexture() const -> FRHITexture* { return DisplayTexture.GetReference(); }
		auto GetWidth() const  -> uint32 { return PreviewWidth; }
		auto GetHeight() const -> uint32 { return PreviewHeight; }
		auto IsValid() const   -> bool { return DisplayTexture != nullptr; }

	private:
		auto UploadPixels(EPixelFormat Format, uint32 Width, uint32 Height, uint32 RowPitch, const uint8* Pixels) -> void;
		auto RefreshDisplayTexture() -> void;
		auto UnregisterDisplayTexture() -> void;

		FTextureRHIRef UploadedTexture;
		FTextureRHIRef FilteredTexture;
		FTextureRHIRef DisplayTexture;
		uint32 PreviewWidth  = 0;
		uint32 PreviewHeight = 0;
		ETexturePreviewChannel SelectedChannel = ETexturePreviewChannel::RGBA;
	};
}
