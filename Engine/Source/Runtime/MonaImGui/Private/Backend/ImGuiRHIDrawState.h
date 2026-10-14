#pragma once

#include "CoreMinimal.h"

namespace Durin::MonaImGui
{
	// Tracks only the state owned by one ImGui render pass. Callbacks invalidate it.
	class FImGuiRHIDrawState
	{
	public:
		auto NeedsRenderStateSetup() const -> bool { return bNeedsSetup; }
		auto Invalidate() -> void
		{
			bNeedsSetup = true;
			bHasScissor = false;
			bHasTexture = false;
		}
		auto OnRenderStateSetup() -> void
		{
			Invalidate();
			bNeedsSetup = false;
		}
		auto ShouldSetScissor(const std::array<float, 4>& Scissor) -> bool
		{
			if (bHasScissor && LastScissor == Scissor) return false;
			LastScissor = Scissor;
			bHasScissor = true;
			return true;
		}
		auto ShouldBindTexture(const void* Texture) -> bool
		{
			if (bHasTexture && LastTexture == Texture) return false;
			LastTexture = Texture;
			bHasTexture = true;
			return true;
		}

	private:
		std::array<float, 4> LastScissor{};
		const void* LastTexture = nullptr;
		bool bNeedsSetup = true;
		bool bHasScissor = false;
		bool bHasTexture = false;
	};
}
