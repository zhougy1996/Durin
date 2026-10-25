#pragma once

#include "CoreMinimal.h"

#include "Math/MathFwd.h"
#include "RHIResources.h"

namespace Durin
{
	// A successful draw names the output generation. A presentation receipt is
	// published only when that texture was consumed by a successful viewport present.
	// Contains no Engine or UObject references and may outlive its producer.
	class FViewportOutputReceipt
	{
	public:
		auto RecordProduced(uint64 Generation) -> void { Produced.store(Generation, std::memory_order_release); }
		auto GetProduced() const -> uint64 { return Produced.load(std::memory_order_acquire); }
		auto RecordPresented(uint64 Generation, const FViewportRHIRef& Viewport) -> void
		{
			if (!Generation || !Viewport) return;
			std::lock_guard Lock(Mutex);
			PresentedGeneration = Generation; PresentingViewport = Viewport;
		}
		auto IsPresented(uint64 Generation, const FRHIViewport* Viewport) const -> bool
		{
			std::lock_guard Lock(Mutex);
			return Generation != 0 && Generation == PresentedGeneration && Viewport && PresentingViewport.GetReference() == Viewport;
		}
		auto GetPresentation() const -> std::pair<uint64, FViewportRHIRef>
		{ std::lock_guard Lock(Mutex); return {PresentedGeneration, PresentingViewport}; }
	private:
		std::atomic<uint64> Produced = 0;
		mutable std::mutex Mutex;
		uint64 PresentedGeneration = 0;
		FViewportRHIRef PresentingViewport;
	};
	// Publishes a size-matched RHI texture for presentation by a UI consumer.
	class IViewportDisplaySource
	{
	public:
		virtual ~IViewportDisplaySource() = default;

		// Makes the resource for the requested physical pixel size observable before this call returns.
		virtual auto PrepareDisplay(const FVector2f& DesiredSize) -> void = 0;

		virtual auto GetDisplayTexture() const -> const FTextureRHIRef& = 0;
		virtual auto GetOutputReceipt() const -> std::shared_ptr<FViewportOutputReceipt> { return {}; }
		// Demand applies to one UI frame. Hidden/clipped consumers do not require
		// the producer to render a full scene just to keep its old texture alive.
		virtual auto OnDisplaySubmitted() -> void {}
	};
}
