#pragma once

#include <functional>
#include "Misc/CoreTypes.h"

namespace Durin::AssetPrivate
{
		struct FPayloadBuildCancelled {};

		// Borrowed for this call only; cancellation never crosses the public codec boundary.
		struct FPayloadBuildControl
		{
			const std::function<bool()>& ShouldCancel;
			uint32 WorkSinceCheckpoint = 0;
			auto Check() const -> void
			{
				if (ShouldCancel && ShouldCancel()) throw FPayloadBuildCancelled{};
			}
			auto Tick() -> void
			{
				if (++WorkSinceCheckpoint < 256) return;
				WorkSinceCheckpoint = 0;
				Check();
			}
		};

}
