#pragma once

#include "RHIResources.h"

#include <array>

namespace Durin::Mona
{
	struct FImGuiRHIImpl_FrameRenderBuffers
	{
		FBufferRHIRef VertexBuffer;
		FBufferRHIRef IndexBuffer;
		FBufferRHIRef ProjectionUniform;
	};

	struct FImGuiRHIImpl_WindowRenderBuffers
	{
		std::array<FImGuiRHIImpl_FrameRenderBuffers, kFrameInFlight> FrameRenderBuffers;

		auto Clear() -> void;
	};

	auto ImGuiRHIImpl_Init() -> void;

	auto ImGuiRHIImpl_Shutdown() -> void;

	auto ImGuiRHIImpl_NewFrame() -> void;

	auto ImGuiRHIImpl_RenderDrawData(const FViewportRHIRef& InViewport, ImDrawData* DrawData, FImGuiRHIImpl_WindowRenderBuffers* WindowRenderBuffers = nullptr) -> void;

}
