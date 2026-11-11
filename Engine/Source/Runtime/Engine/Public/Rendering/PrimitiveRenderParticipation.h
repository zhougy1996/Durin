#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	enum class EMeshCollectionPurpose : uint8
	{
		Receiver,
		Shadow,
	};

	// Copied primitive intent shared by scene publication and PSO collection.
	struct FPrimitiveRenderParticipation
	{
		bool bCastShadow = true;
		bool bRenderInMainPass = true;
		auto IsRelevant(EMeshCollectionPurpose Purpose) const -> bool
		{ return Purpose == EMeshCollectionPurpose::Shadow ? bCastShadow : bRenderInMainPass; }
		auto operator==(const FPrimitiveRenderParticipation&) const -> bool = default;
	};
}
