#pragma once

#include "CoreMinimal.h"

#include "Math/Operations.h"

namespace Durin
{
	// Double-precision admission and float shader data have different validity requirements.
	struct FPrimitiveSceneTransform
	{
		FMatrix LocalToWorld{1.0};
		FMatrix4f NormalToWorld{1.0f};
		FMatrix WorldToLocal{1.0};
		double Determinant = 0.0;
		bool bAdmissible = false;
		bool bValid = false;

		FPrimitiveSceneTransform() = default;
		explicit FPrimitiveSceneTransform(const FMatrix& InLocalToWorld)
			: LocalToWorld(InLocalToWorld)
		{
			if (!Math::TryInverse(LocalToWorld, WorldToLocal)) return;
			bAdmissible = true;
			Determinant = Math::LinearDeterminant(LocalToWorld);
			NormalToWorld = FMatrix4f(WorldToLocal);
			bValid = std::isfinite(Determinant) && Math::IsFinite(FMatrix(NormalToWorld));
		}
	};
}
