#pragma once

#include "RHIResources.h"

namespace Doge::Mona::MonaImGuiBackend
{
	extern ImGuiContext* GMonaImGuiContext;

	auto ImGuiRHIImpl_Init() -> void;

	auto ImGuiRHIImpl_Shutdown() -> void;

	auto ImGuiRHIImpl_NewFrame() -> void;

	auto ImGuiRHIImpl_RenderDrawData(const FViewportRHIRef& InViewport, ImDrawData* DrawData) -> void;

}