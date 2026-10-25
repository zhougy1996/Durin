#pragma once

#include "CoreMinimal.h"
#include "DObject/AssetPath.h"

namespace Durin::AssetPrivate
{
	// Diagnostic only. A callback/codec unit is not made preemptible by its
	// surrounding frame deadline. The threshold selects timing samples, not faults.
	class FScopedAssetLoadTiming
	{
	public:
		FScopedAssetLoadTiming(const FPackagePath& InPath, std::string_view InPhase)
			: Path(InPath), Phase(InPhase) {}
		~FScopedAssetLoadTiming()
		{
			const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
			if (Milliseconds >= 16.0)
				DURIN_DEBUG("Asset timing: {} / {} / {:.2f} ms (owner thread).", Path.ToString(), Phase, Milliseconds);
		}
	private:
		const FPackagePath& Path;
		std::string_view Phase;
		std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
	};
}
