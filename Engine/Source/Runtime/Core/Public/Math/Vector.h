#pragma once

#include "Math/MathFwd.h"

#include <glm/ext.hpp>

namespace Durin
{
	namespace FVectorConstants
	{
		inline constexpr FVector3 Zero(0.0, 0.0, 0.0);
		inline constexpr FVector3 Unit(1.0, 1.0, 1.0);
		inline constexpr FVector3 Up(0.0, 0.0, 1.0);
		inline constexpr FVector3 Forward(1.0, 0.0, 0.0);
		inline constexpr FVector3 Right(0.0, 1.0, 0.0);
	}

	namespace FQuatConstants
	{
		inline constexpr FQuat Identity(1.0, 0.0, 0.0, 0.0);
	}
}
