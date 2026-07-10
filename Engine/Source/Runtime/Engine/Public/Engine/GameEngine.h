#pragma once

#include "Engine/Engine.h"

#include "GameEngine.gen.h"

namespace Durin
{
	DCLASS()
	class DGameEngine : public DEngine
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DGameEngine(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto Init() -> void override;
	};
}
