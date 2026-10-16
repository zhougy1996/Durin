#pragma once

#include "CoreMinimal.h"
#include "ImGuiRHIImpl.h"
#include "RenderingThread.h"
#include "ThirdParty/ImGui/imgui_threaded_rendering.h"

namespace Durin::MonaImGui
{
	// Owns render buffers and alternating draw snapshots for an ImGui viewport.
	struct FImGuiRHIImpl_RendererViewportData
	{
		FImGuiRHIImpl_WindowRenderBuffers RenderBuffers;
		std::array<ImDrawDataSnapshot, 2> DrawDataSnapshots;
		FViewportRHIRef ViewportRHI;
		uint32 NextSnapshotIndex = 0;
	};

	inline auto GetRendererViewportData(ImGuiViewport* Viewport) -> FImGuiRHIImpl_RendererViewportData*
	{
		return Viewport != nullptr ? static_cast<FImGuiRHIImpl_RendererViewportData*>(Viewport->RendererUserData) : nullptr;
	}

	inline auto CreateRendererViewportData(ImGuiViewport* Viewport) -> FImGuiRHIImpl_RendererViewportData*
	{
		if (Viewport == nullptr)
		{
			return nullptr;
		}

		auto* ViewportData = GetRendererViewportData(Viewport);
		if (ViewportData == nullptr)
		{
			ViewportData = new FImGuiRHIImpl_RendererViewportData();
			Viewport->RendererUserData = ViewportData;
		}

		return ViewportData;
	}

	inline auto DestroyRendererViewportData(ImGuiViewport* Viewport) -> void
	{
		if (Viewport == nullptr)
		{
			return;
		}

		if (auto* ViewportData = GetRendererViewportData(Viewport))
		{
			// Queued draws borrow snapshots and render buffers. ImGui invokes this
			// before Mona queues native-window destruction, whose drain is too late.
			FlushRenderingCommands();
			delete ViewportData;
		}
		Viewport->RendererUserData = nullptr;
	}

}
