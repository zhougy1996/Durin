#pragma once

#include "Components/PrimitiveComponent.h"

#include "MeshComponent.gen.h"

namespace Durin
{
	class DMaterialInterface;

	// Provides the common material-slot surface for primitives backed by mesh geometry.
	DCLASS()
	class DMeshComponent : public DPrimitiveComponent
	{
		GENERATED_BODY()
	public:
		// Exposes effective material bindings by stable slot index; mutation fails only for an unavailable slot.
		ENGINE_API virtual auto GetNumMaterials() const -> uint32;
		ENGINE_API virtual auto GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*;
		ENGINE_API virtual auto SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool;
	};
}
