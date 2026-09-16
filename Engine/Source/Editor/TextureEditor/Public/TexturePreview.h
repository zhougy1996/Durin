#pragma once

#include "RHIResources.h"
#include "TextureEditorAPI.h"
#include "Texture/Texture2DData.h"

namespace Durin
{
	struct FTexturePlatformData;
	namespace Image { class FImageView; }
	namespace Mona { class IMonaUIBackend; }
}

namespace Durin::Editor::Texture
{
	enum class ETexturePreviewChannel : uint8
	{
		RGBA,
		Red,
		Green,
		Blue,
		Alpha,
	};

	// Interpretation is independent of channel inspection and Source/Built selection.
	enum class ETexturePreviewInterpretation : uint8 { Auto, Raw, Normal };
	struct FTexturePreviewOptions
	{
		ETextureUsage Usage = ETextureUsage::Color;
		ETexturePreviewInterpretation Interpretation = ETexturePreviewInterpretation::Auto;
		ETexturePreviewChannel Channel = ETexturePreviewChannel::RGBA;
		auto operator==(const FTexturePreviewOptions&) const -> bool = default;
		auto DecodesNormal() const -> bool
		{
			return Channel == ETexturePreviewChannel::RGBA
				&& (Interpretation == ETexturePreviewInterpretation::Normal
					|| (Interpretation == ETexturePreviewInterpretation::Auto && Usage == ETextureUsage::Normal));
		}
	};

	// Render-thread entry point shared by previews and scheduled thumbnail captures.
	// Returns display-encoded SRGBA8, with transparent aspect-fit margins.
	TEXTUREEDITOR_API auto RenderTexturePreview(FRHICommandListImmediate& Commands,
		FRHITexture* Input, uint32 Width, uint32 Height,
		FTexturePreviewOptions Options) -> FTextureRHIRef;

	// Lightweight GPU texture owner for the Texture Editor preview panel.
	// Upload is synchronous (FlushRenderingCommands) because uploads are
	// bounded to one mip per user action and the user expects instant feedback.
	class TEXTUREEDITOR_API FTexturePreview
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
			FTexturePreviewOptions Options = {}
		) -> void;

		// Upload explicitly selected source RGBA8 data (single mip).
		auto UploadSource(
			Image::FImageView Source,
			FTexturePreviewOptions Options = {}
		) -> void;

		// Uploads one editor-generated RGBA8 inspection image.
		auto UploadRGBA8(uint32 Width, uint32 Height,
			FByteView Pixels,
			FTexturePreviewOptions Options = {}) -> void;

		// Change interpretation/channel without re-uploading the input texture.
		auto SetOptions(FTexturePreviewOptions Options) -> void;

		// Retains a built allocation; output size bounds node-preview GPU memory.
		auto SetTexture(FTextureRHIRef Texture, uint32 Width, uint32 Height,
			FTexturePreviewOptions Options = {}) -> void;

		// Release GPU resources and unregister from UI backend.
		auto Release() -> void;

		// Release the shared channel-filter pipeline before the render thread stops.
		static auto ReleaseSharedResources() -> void;

		auto GetTexture() const -> FRHITexture* { return DisplayTexture.GetReference(); }
		auto GetWidth() const  -> uint32 { return PreviewWidth; }
		auto GetHeight() const -> uint32 { return PreviewHeight; }
		auto IsValid() const   -> bool { return DisplayTexture != nullptr; }

	private:
		auto UploadPixels(EPixelFormat Format, uint32 Width, uint32 Height, uint32 RowPitch,
			FByteView Pixels) -> void;
		auto RefreshDisplayTexture() -> void;
		auto UnregisterDisplayTexture() -> void;

		Mona::IMonaUIBackend* RegisteredBackend = nullptr;
		FTextureRHIRef UploadedTexture;
		FTextureRHIRef DisplayTexture;
		uint32 PreviewWidth  = 0;
		uint32 PreviewHeight = 0;
		FTexturePreviewOptions DisplayOptions;
	};
}
