#include "MonaImGui.h"

#include "Backend/MonaImGuiBackend.h"
#include "Widgets/MWindow.h"
#include "MonaGlobals.h"
#include "MonaCoreGlobals.h"

namespace Durin::MonaImGui
{
	namespace
	{
		std::unique_ptr<FMonaImGuiBackend> Backend;
	}

	auto Initialize() -> bool
	{
		if (Backend) return Mona::GetActiveUIBackend() == Backend.get();
		if (!Mona::IsRenderingInitialized() || Mona::GetActiveUIBackend()) return false;

		Backend = std::make_unique<FMonaImGuiBackend>();
		try
		{
			Backend->Initialize();
		}
		catch (...)
		{
			try
			{
				Backend->Shutdown();
			}
			catch (...)
			{
				Backend.reset();
				throw;
			}
			Backend.reset();
			throw;
		}
		if (!Mona::RegisterUIBackend(*Backend))
		{
			Backend->Shutdown();
			Backend.reset();
			return false;
		}
		return true;
	}

	auto Shutdown() -> void
	{
		if (!Backend) return;
		try
		{
			Backend->Shutdown();
		}
		catch (...)
		{
			Mona::UnregisterUIBackend(*Backend);
			Backend.reset();
			throw;
		}
		if (!Mona::UnregisterUIBackend(*Backend))
			throw std::runtime_error("MonaImGui no longer owns the active UI backend.");
		Backend.reset();
	}

	auto DrawTexture(const FRHITexture* Texture, const FVector2f& Size) -> void
	{
		ImGui::Image(reinterpret_cast<ImTextureID>(Texture), {Size.x, Size.y});
	}

	auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		Window->SetTitleBarDarkMode(GetColorTheme() == EColorTheme::Dark);
		FMonaImGuiBackend::Get().BindMainViewportToWindow(Window);
	}

} // namespace Durin::MonaImGui
