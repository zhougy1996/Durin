#include "Components/MeshComponent.h"

namespace Durin
{
	auto DMeshComponent::GetNumMaterials() const -> uint32
	{
		return 0;
	}

	auto DMeshComponent::GetMaterial(uint32) const -> DMaterialInterface*
	{
		return nullptr;
	}

	auto DMeshComponent::SetMaterial(uint32, DMaterialInterface*) -> bool
	{
		return false;
	}

}
