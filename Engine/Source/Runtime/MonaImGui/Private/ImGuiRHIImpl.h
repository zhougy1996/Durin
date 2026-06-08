#pragma once

#include "RHIResources.h"

struct ImDrawData;
struct ImGuiViewport;

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

	auto ImGuiRHIImpl_RegisterTexture(const FTextureRHIRef& Texture) -> void;

	auto ImGuiRHIImpl_UnregisterTexture(const FTextureRHIRef& Texture) -> void;

	auto ImGuiRHIImpl_GetTextureID(const FTextureRHIRef& Texture) -> ImTextureID;

	auto ImGuiRHIImpl_EnsureMainViewportData(ImGuiViewport* Viewport) -> void;

	auto ImGuiRHIImpl_RenderMainViewport(ImGuiViewport* Viewport) -> void;

	auto ImGuiRHIImpl_RenderDrawData(const FViewportRHIRef& InViewport, ImDrawData* DrawData, FImGuiRHIImpl_WindowRenderBuffers* WindowRenderBuffers = nullptr, bool bPresent = true) -> void;

	auto ImGuiRHIImpl_PresentViewport(const FViewportRHIRef& InViewport) -> void;

}
