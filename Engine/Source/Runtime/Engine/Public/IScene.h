#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class DPrimitiveComponent;

	class IScene
	{
	public:
		ENGINE_API IScene() = default;
		ENGINE_API virtual ~IScene() = default;

		virtual auto AddPrimitive(DPrimitiveComponent* Primitive) -> void = 0;

		virtual auto RemovePrimitive(DPrimitiveComponent* Primitive) -> void = 0;

		virtual auto UpdatePrimitiveTransform(DPrimitiveComponent* Primitive) -> void = 0;
	};
}
