#pragma once

namespace Durin::Mona
{
	extern ImGuiContext* GMonaImGuiContext;

	namespace FMonaImGuiBackend
	{
		auto Initialize() -> void;

		auto Shutdown() -> void;

		auto NewFrame() -> void;

		auto Render() -> void;
	};
}