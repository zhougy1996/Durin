#include "Mona/SceneViewport.h"

#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "Rendering/MonaRenderer.h"
#include "Widgets/MWindow.h"
#include "Widgets/MViewport.h"
#include "Application/MonaApplication.h"
#include "RHICommandList.h"

namespace Durin
{
	FSceneViewport::FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<MWindow>& InWindow)
		: FViewport(InViewportClient)
		, RenderMode(Mona::EMonaViewportRenderMode::Window)
		, Window(InWindow)
	{
	}

	FSceneViewport::FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<MViewport>& InViewportWidget, IScene* InRenderScene)
		: FViewport(InViewportClient)
		, RenderMode(Mona::EMonaViewportRenderMode::RenderTarget)
		, ViewportWidget(InViewportWidget)
		, RenderScene(InRenderScene)
	{
	}

	FSceneViewport::~FSceneViewport()
	{
		UnregisterDisplayTexture();
	}

	auto FSceneViewport::UpdateRHIViewport() -> void
	{
		if (RenderMode == Mona::EMonaViewportRenderMode::Window)
		{
			const std::shared_ptr<MWindow> WindowPtr = Window.lock();
			if (WindowPtr == nullptr)
			{
				ViewportRHI = nullptr;
				return;
			}

			if (Mona::FMonaRenderer* Renderer = Mona::FMonaApplication::Get().GetRenderer())
			{
				ViewportRHI = Renderer->GetRHIViewport(*WindowPtr);
			}
			return;
		}

		const FVector2f DesiredSize = GetDesiredSize();
		const uint32 Width = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(DesiredSize.x)));
		const uint32 Height = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(DesiredSize.y)));
		if (RenderTargetRHI == nullptr || RenderTargetRHI->GetSizeX() != Width || RenderTargetRHI->GetSizeY() != Height)
		{
			UnregisterDisplayTexture();
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("SceneViewportRenderTarget", Width, Height, EPixelFormat::SRGBA8_UNORM);
			Desc.AddFlags(ETextureCreateFlags::RenderTargetable);
			RenderTargetRHI = RHICreateTexture(Desc);
			if (Mona::GActiveUIBackend != nullptr)
			{
				Mona::GActiveUIBackend->RegisterTexture(RenderTargetRHI);
			}
		}
		ViewportRHI = nullptr;
	}

	auto FSceneViewport::GetRenderMode() const -> Mona::EMonaViewportRenderMode
	{
		return RenderMode;
	}

	auto FSceneViewport::IsWindowBacked() const -> bool
	{
		return RenderMode == Mona::EMonaViewportRenderMode::Window;
	}

	auto FSceneViewport::GetDesiredSize() const -> FVector2f
	{
		if (RenderMode == Mona::EMonaViewportRenderMode::Window)
		{
			if (const std::shared_ptr<MWindow> WindowPtr = Window.lock())
			{
				return WindowPtr->GetViewportSize();
			}
			return {};
		}

		if (const std::shared_ptr<MViewport> ViewportWidgetPtr = ViewportWidget.lock())
		{
			return ViewportWidgetPtr->GetDesiredSize();
		}

		return {};
	}

	auto FSceneViewport::GetRenderTargetRHI() const -> const FTextureRHIRef&
	{
		return RenderTargetRHI;
	}

	auto FSceneViewport::GetDisplayTexture() const -> const FTextureRHIRef&
	{
		return RenderTargetRHI;
	}

	auto FSceneViewport::UnregisterDisplayTexture() -> void
	{
		if (RenderTargetRHI != nullptr && Mona::GActiveUIBackend != nullptr)
		{
			Mona::GActiveUIBackend->UnregisterTexture(RenderTargetRHI);
		}
	}
}
