#pragma once

namespace Durin
{
	// Defines the module boundary that hosts editor workspaces in the main frame.
	class IMainFrameModule : public IModuleInterface
	{
	public:
		virtual auto CreateDefaultMainFrame() -> void = 0;
	};
}
