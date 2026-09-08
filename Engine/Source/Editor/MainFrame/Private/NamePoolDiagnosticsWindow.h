#pragma once

#include "Misc/Name.h"

namespace Durin::Editor::MainFrame
{
	// Host-owned process-wide diagnostics; sampling stops while hidden or collapsed.
	class FNamePoolDiagnosticsWindow
	{
	public:
		auto Draw(bool& bOpen) -> void;

	private:
		FNamePoolStats Stats;
		double LastSampleTime = -1.0;
		double EntriesPerSecond = 0.0;
		int TargetBlocks = 4;
		bool bAutoRefresh = true;
		bool bWasVisible = false;
	};
}
