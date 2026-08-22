#include "Resources/DefaultTextureResources.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
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
					std::as_bytes(std::span(Color)));
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
						std::as_bytes(std::span(Color)));
				}
			}
			return Texture;
		}

		auto CreateSolidArrayTexture(
			FRHICommandListImmediate& CommandList,
			const char* DebugName) -> FTextureRHIRef
		{
			FRHITextureCreateDesc Desc =
				FRHITextureCreateDesc::Create2DArray(DebugName)
					.SetExtent(1)
					.SetArraySize(3)
					.SetFormat(EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture =
				GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture != nullptr)
			{
				const std::array Transition{FRHITextureTransition::Whole(
					Texture, ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead)};
				CommandList.TransitionTextures(Transition);
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
					std::as_bytes(std::span(Color)));
			}
			return Texture;
		}
	}

	FDefaultTextureResources::FDefaultTextureResources(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator)
	{
	}

	auto FDefaultTextureResources::Initialize_RenderThread(
		FRHICommandListImmediate& CommandList) -> bool
	{
		check(IsInRenderingThread());
		using FResult = TRenderResourceCreateResult<FPayload>;
		return Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[&CommandList]() -> FResult {
				FPayload Candidate;
				Candidate.White = CreateSolidTexture(
					CommandList, "DefaultWhite", {255, 255, 255, 255});
				Candidate.Black = CreateSolidTexture(
					CommandList, "DefaultBlack", {0, 0, 0, 255});
				Candidate.FlatNormal = CreateFlatNormalTexture(CommandList);
				Candidate.BlackCube = CreateSolidCubeTexture(
					CommandList, "DefaultBlackCube", {0, 0, 0, 255});
				Candidate.WhiteArray = CreateSolidArrayTexture(
					CommandList, "DefaultWhiteArray");
				if (Candidate.White == nullptr || Candidate.Black == nullptr
					|| Candidate.FlatNormal == nullptr
					|| Candidate.BlackCube == nullptr
					|| Candidate.WhiteArray == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"DefaultTextureResources",
						"fallback-set",
						"One or more default texture creations returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic) != nullptr;
	}

	auto FDefaultTextureResources::Get_RenderThread(
		EDefaultTexture Texture) const -> FRHITexture*
	{
		check(IsInRenderingThread());
		const FPayload* Payload = Slot.GetPayload();
		if (Payload == nullptr) return nullptr;
		switch (Texture)
		{
		case EDefaultTexture::White: return Payload->White;
		case EDefaultTexture::Black: return Payload->Black;
		case EDefaultTexture::FlatNormal: return Payload->FlatNormal;
		}
		return Payload->White;
	}

	auto FDefaultTextureResources::GetCube_RenderThread() const -> FRHITexture*
	{
		check(IsInRenderingThread());
		const FPayload* Payload = Slot.GetPayload();
		return Payload != nullptr ? Payload->BlackCube.GetReference() : nullptr;
	}

	auto FDefaultTextureResources::GetArray_RenderThread() const -> FRHITexture*
	{
		check(IsInRenderingThread());
		const FPayload* Payload = Slot.GetPayload();
		return Payload != nullptr ? Payload->WhiteArray.GetReference() : nullptr;
	}

	auto FDefaultTextureResources::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		Slot.Reset();
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

	auto GetDefaultArrayTexture_RenderThread() -> FRHITexture*
	{
		check(IsInRenderingThread());
		return GetDefaultTextureResources().GetArray_RenderThread();
	}
} // namespace Durin
