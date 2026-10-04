#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "MetalTexture.h"
#include "MetalResourceDescriptors.h"
#include "RHIResources.h"
#include "RHIPresentation.h"
#include <QuartzCore/QuartzCore.hpp>

namespace Durin
{
	class FMetalViewport final : public FRHIViewport
	{
	public:
		FMetalViewport(NS::SharedPtr<MTL::Device> InDevice, NS::SharedPtr<CA::MetalLayer> InLayer,
			uint32 Width, uint32 Height, EPixelFormat InFormat, EViewportPresentationPolicy InPolicy)
			: Device(std::move(InDevice)), Layer(std::move(InLayer)), Format(InFormat),
			  RequestedPresentationPolicy(InPolicy)
		{
			Layer->setDevice(Device.get());
			Layer->setPixelFormat(ToMetalPixelFormat(Format));
			Layer->setFramebufferOnly(false);
			Resize(Width, Height);
			ApplyRequestedPresentationPolicy();
		}
		~FMetalViewport() override
		{
			const FMetalAutoreleasePool Pool;
			BackBuffer = nullptr;
			Layer.reset();
			Device.reset();
		}
		auto Resize(uint32 Width, uint32 Height) -> bool
		{
			const FMetalAutoreleasePool Pool;
			if (!Width || !Height) return false;
			auto Desc = FRHITextureCreateDesc::Create2D(
				"Metal viewport back buffer", Width, Height, Format);
			Desc.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy
				| ETextureCreateFlags::CPUReadback);
			auto Native = MakeMetalTextureDescriptor(Desc);
			if (!Native) return false;
			auto Texture = NS::TransferPtr(Device->newTexture(Native.get()));
			if (!Texture) return false;
			{
				std::lock_guard Lock(Mutex);
				BackBuffer = new FMetalTexture(Desc, std::move(Texture));
				Layer->setDrawableSize(CGSizeMake(Width, Height));
			}
			return true;
		}
		auto GetBackBuffer(FRHICommandListImmediate&) -> TRefCountPtr<FRHITexture> override
		{ return SnapshotBackBuffer(); }
		auto SnapshotBackBuffer() const -> FTextureRHIRef
		{
			std::lock_guard Lock(Mutex);
			return BackBuffer;
		}
		auto RequestPresentationPolicy(EViewportPresentationPolicy Policy) -> void override
		{ RequestedPresentationPolicy.store(Policy, std::memory_order_relaxed); }
		auto GetPresentMode() const -> EViewportPresentMode override
		{ return PublishedPresentMode.load(std::memory_order_relaxed); }
		// Apply on the native drawing boundary, never on the application-thread request.
		auto ApplyRequestedPresentationPolicy() -> void
		{
			const auto Policy = RequestedPresentationPolicy.load(std::memory_order_relaxed);
			std::lock_guard Lock(Mutex);
			if (AppliedPresentationPolicy == Policy) return;
			const FMetalAutoreleasePool Pool;
			const bool bSync = Policy == EViewportPresentationPolicy::FramePaced;
			Layer->setDisplaySyncEnabled(bSync);
			const bool bActualSync = Layer->displaySyncEnabled();
			PublishedPresentMode.store(BackBuffer
				? (bActualSync ? EViewportPresentMode::Fifo : EViewportPresentMode::Immediate)
				: EViewportPresentMode::Unavailable, std::memory_order_relaxed);
			AppliedPresentationPolicy = Policy;
			DURIN_INFO("Metal viewport presentation: policy={}, requestedSync={}, displaySyncEnabled={}, maximumDrawableCount={}.",
				Policy == EViewportPresentationPolicy::FramePaced ? "FramePaced"
					: Policy == EViewportPresentationPolicy::Unsynchronized ? "Unsynchronized" : "BestEffort",
				bSync, bActualSync, Layer->maximumDrawableCount());
		}
		auto GetLayer() const -> CA::MetalLayer* { return Layer.get(); }
		auto GetFormat() const -> EPixelFormat override { return Format; }
	private:
		NS::SharedPtr<MTL::Device> Device;
		NS::SharedPtr<CA::MetalLayer> Layer;
		EPixelFormat Format;
		mutable std::mutex Mutex;
		FTextureRHIRef BackBuffer;
		std::atomic<EViewportPresentationPolicy> RequestedPresentationPolicy;
		std::optional<EViewportPresentationPolicy> AppliedPresentationPolicy;
		std::atomic<EViewportPresentMode> PublishedPresentMode{EViewportPresentMode::Unavailable};
	};

}
