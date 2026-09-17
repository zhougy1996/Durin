#pragma once

namespace Durin::AssetPrivate
{
	// Capture may retain validated authored bytes before retiring a loose resource.
	// This changes storage residency only, never authored identity or revision.
	class FScopedBulkSaveRetention
	{
		inline static thread_local bool bEnabled = false;
		bool bPrevious;
	public:
		explicit FScopedBulkSaveRetention(bool bRetain) : bPrevious(bEnabled) { bEnabled |= bRetain; }
		~FScopedBulkSaveRetention() { bEnabled = bPrevious; }
		static auto IsEnabled() -> bool { return bEnabled; }
	};
}
