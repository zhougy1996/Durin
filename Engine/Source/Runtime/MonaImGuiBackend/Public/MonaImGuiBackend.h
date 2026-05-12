#pragma once

namespace Doge::Mona
{
	namespace FMonaImGuiBackend
	{
		auto Initialize() -> void;

		auto Shutdown() -> void;

		auto NewFrame() -> void;

		auto Render() -> void;
	};
}