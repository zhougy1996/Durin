#pragma once

namespace Durin::Mona
{
	namespace FMonaImGuiBackend
	{
		auto Initialize() -> void;

		auto Shutdown() -> void;

		auto NewFrame() -> void;

		auto Render() -> void;
	};
}