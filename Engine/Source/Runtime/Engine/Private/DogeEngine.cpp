#include "DogeEngine.h"

#include "Misc/Time.h"

namespace Doge
{
	float GAverageFPS = 0.0f;
	float GAverageMS = 0.0f;

	auto CalculateFPSTimings() -> void
	{
		static double LastTime = 0.0;
		double CurrentTime = FTime::Seconds();
		float FrameTimeMS = static_cast<float>((CurrentTime - LastTime) * 1000.0);
		LastTime = CurrentTime;

		GAverageMS = GAverageMS * 0.75f + FrameTimeMS * 0.25f;
		GAverageFPS = 1000.0f / GAverageMS;
	}
}