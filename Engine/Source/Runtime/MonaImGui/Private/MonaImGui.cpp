#include "MonaImGui.h"

#include "MonaCoreGlobals.h"
#include "MonaImGuiBackend.h"

namespace Durin::MonaImGui
{
	namespace
	{
		auto GetActiveImGuiBackend() -> Mona::FMonaImGuiBackend*
		{
			return static_cast<Mona::FMonaImGuiBackend*>(Mona::GActiveUIBackend);
		}
	}

	auto DrawTexture(FRHITexture* Texture, const FVector2f& Size) -> bool
	{
		if (Mona::FMonaImGuiBackend* Backend = GetActiveImGuiBackend())
		{
			return Backend->DrawTexture(Texture, Size);
		}

		return false;
	}

	auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		if (Mona::FMonaImGuiBackend* Backend = GetActiveImGuiBackend())
		{
			Backend->BindMainViewportToWindow(Window);
		}
	}

	auto ShowDemoWindow() -> void
	{
		if (Mona::FMonaImGuiBackend* Backend = GetActiveImGuiBackend())
		{
			Backend->ShowDemoWindow();
		}
	}
}
