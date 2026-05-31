#pragma once

#include "Engine/Engine.h"

namespace Durin
{
	class DGameEngine : public DEngine
	{
	public:
		ENGINE_API auto Init() -> void override;
	};
}
