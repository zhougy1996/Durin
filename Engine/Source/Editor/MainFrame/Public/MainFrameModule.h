#pragma once

#include "Interfaces/IMainFrameModule.h"

namespace Doge
{
	class FMainFrameModule : public IMainFrameModule
	{
	public:
		FMainFrameModule() = default;
		~FMainFrameModule() = default;

		auto StartupModule() -> void override;
		auto ShutdownModule() -> void override;
		auto CreateDefaultMainFrame() -> void override;
	};
}
