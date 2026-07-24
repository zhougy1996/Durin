#pragma once

#include "Components/PrimitiveComponent.h"

#include "MeshComponent.gen.h"

namespace Durin
{
	// Marks primitive components whose render proxy is backed by mesh geometry.
	DCLASS()
	class DMeshComponent : public DPrimitiveComponent
	{
		GENERATED_BODY()
	};
}
