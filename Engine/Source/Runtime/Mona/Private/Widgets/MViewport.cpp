#include "Widgets/MViewport.h"

#include "MonaUIBackend.h"

namespace Durin
{
	auto MViewport::SetDesiredSize(const FVector2f& InDesiredSize) -> void
	{
		DesiredSize = InDesiredSize;
	}

	auto MViewport::GetDesiredSize() const -> FVector2f
	{
		return DesiredSize;
	}

	auto MViewport::SetViewportInterface(const std::shared_ptr<Mona::IMonaViewport>& InViewport) -> void
	{
		ViewportInterface = InViewport;
	}

	auto MViewport::GetViewportInterface() const -> std::shared_ptr<Mona::IMonaViewport>
	{
		return ViewportInterface.lock();
	}

	auto MViewport::WasTextureDrawn() const -> bool
	{
		return bLastDrawSucceeded;
	}

	auto MViewport::Draw() -> void
	{
		bLastDrawSucceeded = false;

		const std::shared_ptr<Mona::IMonaViewport> ViewportPtr = ViewportInterface.lock();
		if (ViewportPtr == nullptr)
		{
			return;
		}

		ViewportPtr->UpdateRHIViewport();
		if (ViewportPtr->IsWindowBacked())
		{
			return;
		}

		UpdateDisplayedRenderTarget();

		bLastDrawSucceeded = MonaUI::DrawTexture(DisplayedRenderTarget.GetReference(), DesiredSize);
	}

	auto MViewport::UpdateDisplayedRenderTarget() -> void
	{
		const std::shared_ptr<Mona::IMonaViewport> ViewportPtr = ViewportInterface.lock();
		if (ViewportPtr == nullptr)
		{
			return;
		}

		FRHITexture* RenderTargetTexture = ViewportPtr->GetRenderTargetTexture();
		if (ViewportPtr->IsRenderTargetReady() && RenderTargetTexture != nullptr)
		{
			DisplayedRenderTarget = RenderTargetTexture;
		}
	}
}
