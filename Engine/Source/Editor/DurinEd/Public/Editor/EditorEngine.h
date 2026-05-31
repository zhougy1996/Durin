#pragma once

#include "DurinEdAPI.h"
#include "Engine/Engine.h"

namespace Durin
{
	class DEditorEngine : public DEngine
	{
	public:
		DURINED_API auto Init() -> void override;
	};
}
