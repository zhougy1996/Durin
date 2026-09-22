#pragma once

#include "RDG/RDGDiagnostics.h"

#include <array>

namespace Durin
{
	// Renderer-owned, render-thread-only history shared by all view submissions.
	class FSceneRenderGraphWarnings final
	{
	public:
		static constexpr uint32 MaxWarnings = 32;

		auto ShouldReport(const FRDGStatistics& Statistics, const FRDGBudget& Budget) -> bool
		{
			if (!Statistics.IsStructuralRegressionBudgetExceeded()
				|| Count == MaxWarnings) return false;
			const FSignature Signature{
				Statistics.DeclaredPasses, Budget.RegressionMaxPasses,
				Statistics.Dependencies, Budget.RegressionMaxDependencies,
				Statistics.BufferTransitions, Budget.RegressionMaxBufferTransitions,
				Statistics.TextureTransitions, Budget.RegressionMaxTextureTransitions,
				Statistics.TextureTransitionSubresources};
			for (uint32 Index = 0; Index < Count; ++Index)
				if (Reported[Index] == Signature) return false;
			Reported[Count++] = Signature;
			return true;
		}

		auto IsFull() const -> bool { return Count == MaxWarnings; }

	private:
		using FSignature = std::array<uint32, 9>;
		std::array<FSignature, MaxWarnings> Reported{};
		uint32 Count = 0;
	};
} // namespace Durin
