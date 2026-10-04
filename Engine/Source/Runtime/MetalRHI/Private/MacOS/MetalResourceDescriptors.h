#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"

#include <Metal/Metal.hpp>

namespace Durin
{
	auto IsMetalColorRenderFormat(EPixelFormat Format) -> bool;
	auto ToMetalPixelFormat(EPixelFormat Format) -> MTL::PixelFormat;
	auto MakeMetalTextureDescriptor(const FRHITextureCreateDesc& Desc)
		-> NS::SharedPtr<MTL::TextureDescriptor>;
	auto MakeMetalSamplerDescriptor(const FRHISamplerDesc& Desc)
		-> NS::SharedPtr<MTL::SamplerDescriptor>;
	// Source is borrowed for creation; the caller's RHI view retains its parent.
	auto MakeMetalTextureView(MTL::Texture* Source, const FRHITextureViewDesc& Desc)
		-> NS::SharedPtr<MTL::Texture>;
}
