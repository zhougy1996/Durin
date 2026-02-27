#pragma once

namespace Doge::Mona
{
	class MWindow;

	class MONACORE_API FMonaRenderer
	{
	public:
		virtual auto CreateViewport(const TSharedPtr<MWindow>& Window) -> void = 0;
	};
}