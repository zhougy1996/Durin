#pragma once

#include "Math/Vector.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"
#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListImmediate;

	// Owns the fullscreen triangle geometry and vertex layout shared by
	// screen-space passes.
	class RENDERER_API FFullscreenGeometryResources
	{
	public:
		struct FVertex
		{
			FVector2f Position;
			FVector2f UV;
		};

		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList) -> bool;
		auto GetVertexDeclaration_RenderThread() const
			-> const FVertexDeclarationRHIRef&;
		auto GetVertexBuffer_RenderThread() const -> const FBufferRHIRef&;
		auto GetIndexBuffer_RenderThread() const -> const FBufferRHIRef&;
		auto RetryFailedResources_RenderThread() -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FPayload
		{
			FVertexDeclarationRHIRef VertexDeclaration;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
		};

		FRenderResourceGeneration Generation;
		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Device};
	};

} // namespace Durin
