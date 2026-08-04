#include "Resources/DefaultTextureResources.h"

#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		FDefaultTextureResources* GActiveDefaultTextureResources = nullptr;

		auto CreateSolidTexture(
			FRHICommandListImmediate& CommandList,
			const char* DebugName,
			const std::array<uint8, 4>& Color) -> FTextureRHIRef
		{
			FRHITextureCreateDesc Desc =
				FRHITextureCreateDesc::Create2D(
					DebugName, 1, 1, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture =
				GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture != nullptr)
			{
				const FUpdateTextureRegion2D Region(
					0, 0, 0, 0, 1, 1);
				GDynamicRHI->RHIUpdateTexture2D(
					CommandList,
					Texture,
					0,
					0,
					Region,
					4,
					Color.data());
			}
			return Texture;
		}

		auto CreateSolidCubeTexture(
			FRHICommandListImmediate& CommandList,
			const char* DebugName,
			const std::array<uint8, 4>& Color) -> FTextureRHIRef
		{
			FRHITextureCreateDesc Desc =
				FRHITextureCreateDesc::CreateCube(DebugName)
				.SetExtent(1)
				.SetFormat(EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture =
				GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture != nullptr)
			{
				const FUpdateTextureRegion2D Region(
					0, 0, 0, 0, 1, 1);
				for (uint32 ArraySlice = 0;
					ArraySlice < TextureCubeFaceCount;
					++ArraySlice)
				{
					GDynamicRHI->RHIUpdateTexture2D(
						CommandList,
						Texture,
						0,
						ArraySlice,
						Region,
						4,
						Color.data());
				}
			}
			return Texture;
		}

		auto CreateFlatNormalTexture(
			FRHICommandListImmediate& CommandList) -> FTextureRHIRef
		{
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
				"DefaultFlatNormal", 1, 1, EPixelFormat::RGBA32_FLOAT)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture =
				GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture != nullptr)
			{
				const std::array<float, 4> Color{0.5f, 0.5f, 1.0f, 1.0f};
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, 1, 1);
				GDynamicRHI->RHIUpdateTexture2D(
					CommandList,
					Texture,
					0,
					0,
					Region,
					sizeof(Color),
					reinterpret_cast<const uint8*>(Color.data()));
			}
			return Texture;
		}
	}

	auto FDefaultTextureResources::Initialize_RenderThread(
		FRHICommandListImmediate& CommandList) -> void
	{
		check(IsInRenderingThread());
		if (White != nullptr)
		{
			return;
		}
		White = CreateSolidTexture(
			CommandList, "DefaultWhite", {255, 255, 255, 255});
		Black = CreateSolidTexture(
			CommandList, "DefaultBlack", {0, 0, 0, 255});
		FlatNormal = CreateFlatNormalTexture(CommandList);
		BlackCube = CreateSolidCubeTexture(
			CommandList, "DefaultBlackCube", {0, 0, 0, 255});
	}

	auto FDefaultTextureResources::Get_RenderThread(
		EDefaultTexture Texture) const -> FRHITexture*
	{
		check(IsInRenderingThread());
		switch (Texture)
		{
		case EDefaultTexture::White: return White;
		case EDefaultTexture::Black: return Black;
		case EDefaultTexture::FlatNormal: return FlatNormal;
		}
		return White;
	}

	auto FDefaultTextureResources::GetCube_RenderThread() const -> FRHITexture*
	{
		check(IsInRenderingThread());
		return BlackCube;
	}

	auto FDefaultTextureResources::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		White = nullptr;
		Black = nullptr;
		FlatNormal = nullptr;
		BlackCube = nullptr;
	}

	auto SetActiveDefaultTextureResources(
		FDefaultTextureResources* Resources) -> void
	{
		GActiveDefaultTextureResources = Resources;
	}

	auto GetDefaultTextureResources() -> FDefaultTextureResources&
	{
		check(GActiveDefaultTextureResources != nullptr);
		return *GActiveDefaultTextureResources;
	}

	auto GetDefaultTexture_RenderThread(EDefaultTexture Texture) -> FRHITexture*
	{
		check(IsInRenderingThread());
		return GetDefaultTextureResources().Get_RenderThread(Texture);
	}

	auto ResolveTexture_RenderThread(
		const FRHITextureReferenceRef& TextureReference,
		EDefaultTexture Fallback) -> FRHITexture*
	{
		check(IsInRenderingThread());
		if (TextureReference != nullptr)
		{
			if (FRHITexture* Texture =
					TextureReference->GetReferencedTexture_RenderThread())
			{
				return Texture;
			}
		}
		return GetDefaultTexture_RenderThread(Fallback);
	}

	auto GetDefaultCubeTexture_RenderThread() -> FRHITexture*
	{
		check(IsInRenderingThread());
		return GetDefaultTextureResources().GetCube_RenderThread();
	}
} // namespace Durin
