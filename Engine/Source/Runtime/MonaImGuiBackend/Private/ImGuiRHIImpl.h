#pragma once

namespace Doge::Mona::ImGuiBackend
{
	auto ImGuiRHIImpl_Init() -> void;

	auto ImGuiRHIImpl_Shutdown() -> void;

	auto ImGuiRHIImpl_NewFrame() -> void;

	auto ImGuiRHIImpl_RenderDrawData(ImDrawData* DrawData) -> void;

	auto ImGuiRHIImpl_UpdateTexture(ImTextureData* TextureData) -> void;
}