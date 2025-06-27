#pragma once

#include "Math/MathFwd.h"

#include <glm/glm.hpp>
#include <glm/ext.hpp>

namespace FVectorConstants
{
inline constexpr FVector Zero(0.0, 0.0, 0.0);
inline constexpr FVector Unit(1.0, 1.0, 1.0);
inline constexpr FVector Up(0.0, 0.0, 1.0);
inline constexpr FVector Forward(1.0, 0.0, 0.0);
inline constexpr FVector Right(0.0, 1.0, 0.0);
} // namespace FVectorConstants

namespace FQuatConstants
{
inline constexpr FQuat Identity{};
} // namespace FQuatConstants