#include "ImGuiRHIImpl.h"

#include "DynamicRHI.h"

namespace Doge::Mona::ImGuiBackend
{


	auto ImGuiRHIImpl_Init() -> void
	{
		ImGuiIO& IO = ImGui::GetIO();
		IMGUI_CHECKVERSION();
		assert(GDynamicRHI); // Make sure the RHI is initialized before initializing the ImGui RHI backend

		IO.BackendRendererUserData = GDynamicRHI;
		IO.BackendRendererName = "RHI";
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // We can honor ImGuiPlatformIO::Textures[] requests during render.
	}

	auto ImGuiRHIImpl_Shutdown() -> void
	{
		ImGuiIO& IO = ImGui::GetIO();
		IO.BackendRendererUserData = nullptr;
		IO.BackendRendererName = nullptr;
		IO.BackendFlags&= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
	}

	auto ImGuiRHIImpl_NewFrame() -> void
	{
	}

	auto ImGuiRHIImpl_RenderDrawData(ImDrawData* DrawData) -> void
	{
		int FrameBufferWidth = static_cast<int>(DrawData->DisplaySize.x * DrawData->FramebufferScale.x);
		int FrameBufferHeight = static_cast<int>(DrawData->DisplaySize.y * DrawData->FramebufferScale.y);
		if (FrameBufferWidth <= 0 || FrameBufferHeight <= 0)
		{
			return;
		}

		if (DrawData->Textures != nullptr)
			for (ImTextureData* TextureData : *DrawData->Textures)
				if (TextureData->Status != ImTextureStatus_OK)
					ImGuiRHIImpl_UpdateTexture(TextureData);
	}

	auto ImGuiRHIImpl_UpdateTexture(ImTextureData* TextureData) -> void
	{
	}
} // namespace Doge::Mona::ImGuiBackend