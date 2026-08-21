#include "EditorBranding.h"

#include "DynamicRHI.h"
#include "Image/ImageDecoder.h"
#include "Misc/Paths.h"
#include "MonaCoreGlobals.h"
#include "MonaImGui.h"
#include "MonaUIBackend.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin::Editor::MainFrame
{
	namespace
	{
		struct FBrandMip
		{
			std::vector<uint8> Pixels;
			uint32 Width = 0;
			uint32 Height = 0;
		};

		auto BuildNextBrandMip(const FBrandMip& Source) -> FBrandMip
		{
			FBrandMip Result;
			Result.Width = std::max(Source.Width / 2, 1u);
			Result.Height = std::max(Source.Height / 2, 1u);
			Result.Pixels.resize(static_cast<size_t>(Result.Width) * Result.Height * 4);
			for (uint32 Y = 0; Y < Result.Height; ++Y)
			{
				for (uint32 X = 0; X < Result.Width; ++X)
				{
					uint32 AlphaSum = 0;
					std::array<uint32, 3> PremultipliedColor{};
					uint32 SampleCount = 0;
					for (uint32 OffsetY = 0; OffsetY < 2; ++OffsetY)
					{
						const uint32 SourceY = std::min(Y * 2 + OffsetY, Source.Height - 1);
						for (uint32 OffsetX = 0; OffsetX < 2; ++OffsetX)
						{
							const uint32 SourceX = std::min(X * 2 + OffsetX, Source.Width - 1);
							const size_t SourceOffset = (static_cast<size_t>(SourceY) * Source.Width + SourceX) * 4;
							const uint32 Alpha = Source.Pixels[SourceOffset + 3];
							AlphaSum += Alpha;
							for (uint32 Channel = 0; Channel < 3; ++Channel)
								PremultipliedColor[Channel] += Source.Pixels[SourceOffset + Channel] * Alpha;
							++SampleCount;
						}
					}

					const size_t DestinationOffset = (static_cast<size_t>(Y) * Result.Width + X) * 4;
					for (uint32 Channel = 0; Channel < 3; ++Channel)
						Result.Pixels[DestinationOffset + Channel] = AlphaSum > 0
							? static_cast<uint8>((PremultipliedColor[Channel] + AlphaSum / 2) / AlphaSum)
							: 0;
					Result.Pixels[DestinationOffset + 3] = static_cast<uint8>((AlphaSum + SampleCount / 2) / SampleCount);
				}
			}
			return Result;
		}

		auto BuildBrandMips(Image::FDecodedImage&& Image) -> std::vector<FBrandMip>
		{
			std::vector<FBrandMip> Mips;
			Mips.push_back({std::move(Image.Pixels), Image.Width, Image.Height});
			while (Mips.back().Width > 1 || Mips.back().Height > 1)
				Mips.push_back(BuildNextBrandMip(Mips.back()));
			return Mips;
		}

		struct FBrandUploadState
		{
			std::mutex Mutex;
			FTextureRHIRef PendingTexture;
			bool bAcceptingResult = true;
		};
	}

	struct FEditorBrandTexture::FImpl
	{
		std::shared_ptr<FBrandUploadState> UploadState = std::make_shared<FBrandUploadState>();
		FTextureRHIRef Texture;
	};

	FEditorBrandTexture::FEditorBrandTexture()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FEditorBrandTexture::~FEditorBrandTexture()
	{
		{
			std::lock_guard Lock(Impl->UploadState->Mutex);
			Impl->UploadState->bAcceptingResult = false;
			Impl->UploadState->PendingTexture = nullptr;
		}
		if (Impl->Texture && Mona::GetActiveUIBackend())
			Mona::GetActiveUIBackend()->UnregisterTexture(Impl->Texture);
	}

	auto FEditorBrandTexture::Load(std::string& OutError) -> bool
	{
		Image::FDecodedImage Image;
		const std::string SourcePath = FPaths::EngineContentDir() + "Editor/Branding/DurinEditorLogoUI.png";
		if (!Image::DecodeImageFromFile(
				SourcePath,
				Image,
				OutError,
				{256ull * 1024ull, 256ull * 256ull}))
			return false;
		if (Image.Width == 0 || Image.Height == 0)
		{
			OutError = "The editor branding image has no pixels.";
			return false;
		}

		auto Mips = std::make_shared<std::vector<FBrandMip>>(BuildBrandMips(std::move(Image)));
		const std::weak_ptr<FBrandUploadState> WeakState = Impl->UploadState;
		ENQUEUE_RENDER_COMMAND(UploadEditorBrandTexture)([WeakState, Mips](FRHICommandListImmediate& CommandList) {
			const FBrandMip& BaseMip = Mips->front();
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
				"EditorBrand", BaseMip.Width, BaseMip.Height, EPixelFormat::SRGBA8_UNORM)
				.SetNumMips(static_cast<uint8>(Mips->size()))
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture)
			{
				for (uint32 MipIndex = 0; MipIndex < Mips->size(); ++MipIndex)
				{
					const FBrandMip& Mip = (*Mips)[MipIndex];
					GDynamicRHI->RHIUpdateTexture2D(
						CommandList, Texture, MipIndex, 0,
						FUpdateTextureRegion2D(0, 0, 0, 0, Mip.Width, Mip.Height),
						Mip.Width * 4, Mip.Pixels.data());
				}
			}
			if (const std::shared_ptr<FBrandUploadState> State = WeakState.lock())
			{
				std::lock_guard Lock(State->Mutex);
				if (State->bAcceptingResult) State->PendingTexture = std::move(Texture);
			}
		});
		return true;
	}

	auto FEditorBrandTexture::UpdateAndGetTexture() -> const FRHITexture*
	{
		FTextureRHIRef UploadedTexture;
		{
			std::lock_guard Lock(Impl->UploadState->Mutex);
			UploadedTexture = std::move(Impl->UploadState->PendingTexture);
		}
		if (UploadedTexture && Mona::GetActiveUIBackend())
		{
			Mona::GetActiveUIBackend()->RegisterTexture(UploadedTexture);
			Impl->Texture = std::move(UploadedTexture);
		}
		return Impl->Texture.GetReference();
	}

	auto DrawEditorBrandMark(
		ImDrawList* DrawList,
		const FRHITexture* Texture,
		const ImVec2& Min,
		float Size) -> void
	{
		if (Texture)
		{
			DrawList->AddImage(
				reinterpret_cast<ImTextureID>(Texture),
				Min,
				Min + ImVec2(Size, Size));
			return;
		}

		const float Width = Size * 0.82f;
		const auto Point = [&](float X, float Y) { return Min + ImVec2(Width * X, Size * Y); };
		const std::array Outer = {
			Point(0.00f, 0.00f), Point(0.63f, 0.00f), Point(1.00f, 0.30f), Point(1.00f, 0.70f),
			Point(0.65f, 1.00f), Point(0.00f, 1.00f), Point(0.00f, 0.77f), Point(0.63f, 0.77f),
			Point(0.76f, 0.62f), Point(0.76f, 0.35f), Point(0.62f, 0.23f), Point(0.00f, 0.23f)};
		DrawList->AddConcavePolyFilled(Outer.data(), static_cast<int>(Outer.size()), IM_COL32(24, 104, 232, 255));
		const std::array Highlight = {
			Point(0.00f, 0.00f), Point(0.63f, 0.00f), Point(1.00f, 0.30f), Point(1.00f, 0.46f),
			Point(0.76f, 0.35f), Point(0.62f, 0.23f), Point(0.00f, 0.23f)};
		DrawList->AddConcavePolyFilled(Highlight.data(), static_cast<int>(Highlight.size()), IM_COL32(28, 193, 235, 255));
		const std::array Wedge = {
			Point(0.00f, 0.17f), Point(0.29f, 0.42f), Point(0.29f, 0.66f), Point(0.00f, 0.83f)};
		DrawList->AddConvexPolyFilled(Wedge.data(), static_cast<int>(Wedge.size()), IM_COL32(137, 61, 226, 255));
	}
}
