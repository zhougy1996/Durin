#pragma once
#include "IMonaUIBackend.h"
#include "MonaImGuiBackendAPI.h"

struct ImGuiContext;

namespace Durin::Mona
{
	class MWindow;
	extern MONAIMGUIBACKEND_API ImGuiContext* GMonaImGuiContext;

	class MONAIMGUIBACKEND_API FMonaImGuiUIBackend final : public IMonaUIBackend
	{
	public:
		auto Initialize() -> void override;

		auto Shutdown() -> void override;

		auto NewFrame() -> void override;

		auto Render() -> void override;

		auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void override;
	};

	namespace FMonaImGuiBackend
	{
		MONAIMGUIBACKEND_API auto Initialize() -> void;

		MONAIMGUIBACKEND_API auto Shutdown() -> void;

		MONAIMGUIBACKEND_API auto NewFrame() -> void;

		MONAIMGUIBACKEND_API auto Render() -> void;

		MONAIMGUIBACKEND_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;

		MONAIMGUIBACKEND_API auto ShowDemoWindow() -> void;
	};
}
