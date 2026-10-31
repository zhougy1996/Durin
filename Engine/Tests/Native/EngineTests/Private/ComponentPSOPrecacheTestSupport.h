#pragma once

#include "CoreMinimal.h"
#include "PSOPrecacheComponent.h"

namespace Durin::Tests
{
	class FScopedComponentPSOPrecacheMode
	{
	public:
		explicit FScopedComponentPSOPrecacheMode(EComponentPSOPrecacheMode Mode)
			: Previous(GetComponentPSOPrecacheMode()) { SetComponentPSOPrecacheMode(Mode); }
		~FScopedComponentPSOPrecacheMode() { SetComponentPSOPrecacheMode(Previous); }
		FScopedComponentPSOPrecacheMode(const FScopedComponentPSOPrecacheMode&) = delete;
		auto operator=(const FScopedComponentPSOPrecacheMode&) -> FScopedComponentPSOPrecacheMode& = delete;
	private:
		EComponentPSOPrecacheMode Previous;
	};
}
