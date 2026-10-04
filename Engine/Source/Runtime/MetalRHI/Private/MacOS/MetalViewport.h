#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "MetalTexture.h"
#include "MetalResourceDescriptors.h"
#include "RHIResources.h"
#include <QuartzCore/QuartzCore.hpp>

namespace Durin
{
	class FMetalViewport final : public FRHIViewport
	{
	public:
		FMetalViewport(NS::SharedPtr<MTL::Device> InDevice, NS::SharedPtr<CA::MetalLayer> InLayer,
			uint32 Width, uint32 Height, EPixelFormat InFormat)
			: Device(std::move(InDevice)), Layer(std::move(InLayer)), Format(InFormat)
		{
			Layer->setDevice(Device.get());
			Layer->setPixelFormat(ToMetalPixelFormat(Format));
			Layer->setFramebufferOnly(false);
			Resize(Width, Height);
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
		auto GetLayer() const -> CA::MetalLayer* { return Layer.get(); }
		auto GetFormat() const -> EPixelFormat override { return Format; }
	private:
		NS::SharedPtr<MTL::Device> Device;
		NS::SharedPtr<CA::MetalLayer> Layer;
		EPixelFormat Format;
		mutable std::mutex Mutex;
		FTextureRHIRef BackBuffer;
	};

}
