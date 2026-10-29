#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Copied primitive intent shared by scene publication and PSO collection.
	struct FPrimitiveRenderParticipation
	{
		bool bCastShadow = true;
		bool bRenderInMainPass = true;
		auto IsRelevant(bool bShadowPass) const -> bool
		{ return bShadowPass ? bCastShadow : bRenderInMainPass; }
		auto operator==(const FPrimitiveRenderParticipation&) const -> bool = default;
	};
}
