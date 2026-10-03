#include "Widgets/MViewport.h"

#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"

namespace Durin
{
	MViewport::~MViewport()
	{
		ReleaseRegisteredTexture();
	}

	auto MViewport::SetDesiredSize(const FVector2f& InDesiredSize) -> void
	{
		DesiredSize = InDesiredSize;
	}

	auto MViewport::GetDesiredSize() const -> FVector2f
	{
		return DesiredSize;
	}

	auto MViewport::SetDisplaySource(const std::shared_ptr<IViewportDisplaySource>& InDisplaySource) -> void
	{
		if (DisplaySource.lock() == InDisplaySource
			&& (InDisplaySource != nullptr || RegisteredTexture == nullptr)) return;
		ReleaseRegisteredTexture();
		DisplaySource = InDisplaySource;
	}

	auto MViewport::GetDisplaySource() const -> std::shared_ptr<IViewportDisplaySource>
	{
		return DisplaySource.lock();
	}

	auto MViewport::WasTextureDrawn() const -> bool
	{
		return bLastDrawSucceeded;
	}

	auto MViewport::Draw() -> void
	{
		bLastDrawSucceeded = false;

		const std::shared_ptr<IViewportDisplaySource> DisplaySourcePtr = DisplaySource.lock();
		if (DisplaySourcePtr == nullptr)
		{
			ReleaseRegisteredTexture();
			return;
		}

		const Mona::IMonaUIBackend* Backend = Mona::GetActiveUIBackend();
		const FVector2f FramebufferScale = Backend != nullptr
			? Backend->GetFramebufferScale() : FVector2f(1.0f, 1.0f);
		DisplaySourcePtr->PrepareDisplay({
			DesiredSize.x * FramebufferScale.x, DesiredSize.y * FramebufferScale.y});
		const FTextureRHIRef& DisplayTexture = DisplaySourcePtr->GetDisplayTexture();
		SynchronizeRegisteredTexture(DisplayTexture);
		if (DisplayTexture == nullptr || RegisteredBackend == nullptr)
		{
			return;
		}

		bLastDrawSucceeded = RegisteredBackend->DrawImage(DisplayTexture, DesiredSize);
	}

	auto MViewport::SynchronizeRegisteredTexture(const FTextureRHIRef& DisplayTexture) -> void
	{
		Mona::IMonaUIBackend* ActiveBackend = Mona::GetActiveUIBackend();
		if (RegisteredTexture == DisplayTexture && RegisteredBackend == ActiveBackend) return;

		ReleaseRegisteredTexture();
		if (DisplayTexture != nullptr && ActiveBackend != nullptr)
		{
			ActiveBackend->RegisterTexture(DisplayTexture);
			RegisteredTexture = DisplayTexture;
			RegisteredBackend = ActiveBackend;
		}
	}

	auto MViewport::ReleaseRegisteredTexture() -> void
	{
		if (RegisteredTexture != nullptr && RegisteredBackend != nullptr
			&& RegisteredBackend == Mona::GetActiveUIBackend())
		{
			RegisteredBackend->UnregisterTexture(RegisteredTexture);
		}
		RegisteredTexture = nullptr;
		RegisteredBackend = nullptr;
	}
}
