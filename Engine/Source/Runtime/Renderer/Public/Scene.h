#pragma once

#include "IScene.h"

class FScene : public IScene
{
	auto AddPrimitive(DPrimitiveComponent* Primitive) -> void override
	{
		// Implementation for adding a primitive to the scene
	}

	auto RemovePrimitive(DPrimitiveComponent* Primitive) -> void override
	{
		// Implementation for removing a primitive from the scene
	}

	auto UpdatePrimitiveTransform(DPrimitiveComponent* Primitive) -> void override
	{
		// Implementation for updating the transform of a primitive in the scene
	}

};