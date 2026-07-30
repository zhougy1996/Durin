#pragma once

#include "Math/Vector.h"
#include "RenderResourceCreation.h"
#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListImmediate;

	// Owns the fullscreen triangle shared by post process and editor grid.
	class FFullscreenGeometryResources
	{
	public:
		struct FVertex
		{
			FVector2f Position;
			FVector2f UV;
		};

		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList) -> bool;
		auto GetVertexBuffer_RenderThread() const -> const FBufferRHIRef&;
		auto GetIndexBuffer_RenderThread() const -> const FBufferRHIRef&;
		auto RetryFailedResources_RenderThread() -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FPayload
		{
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
		};

		FRenderResourceGeneration Generation;
		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Device};
	};

	// Returns the shared geometry owner installed by the active renderer module.
	auto GetFullscreenGeometryResources()
		-> FFullscreenGeometryResources&;
	auto SetActiveFullscreenGeometryResources(
		FFullscreenGeometryResources* Resources) -> void;
} // namespace Durin
