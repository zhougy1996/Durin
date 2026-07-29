#pragma once

#include "Math/Vector.h"
#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListImmediate;
}

namespace Durin::RendererFullscreenGeometry
{
	struct FVertex
	{
		FVector2f Position;
		FVector2f UV;
	};

	auto EnsureResources(FRHICommandListImmediate& CommandList) -> bool;
	auto GetVertexBuffer() -> const FBufferRHIRef&;
	auto GetIndexBuffer() -> const FBufferRHIRef&;
	auto ReleaseResources() -> void;
} // namespace Durin::RendererFullscreenGeometry
