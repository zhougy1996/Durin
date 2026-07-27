#pragma once

#include "Interfaces/IMainFrameModule.h"

namespace Durin
{
	// Owns the editor host window and workspace manager for the process.
	class FMainFrameModule : public IMainFrameModule
	{
	public:
		FMainFrameModule() = default;
		~FMainFrameModule() = default;

		auto StartupModule() -> void override;
		auto ShutdownModule() -> void override;
		auto CreateDefaultMainFrame() -> void override;
		auto RequestOpenAssetUpgradeCenter() -> void override;

	private:
		bool bAssetUpgradeCenterOpenRequested = false;
	};
}
