#pragma once

#include <glm/fwd.hpp>

namespace Durin
{
	template<typename T> struct TRotator;

	using FReal = double;
	using FMatrix = glm::dmat4x4;
	using FQuat = glm::dquat;

	struct FTransform;
	struct FBox;
	using FRotator = TRotator<FReal>;

	using FVector2f = glm::vec2;
	using FVector3f = glm::vec3;
	using FVector4f = glm::vec4;

	using FVector2d = glm::dvec2;
	using FVector3d = glm::dvec3;
	using FVector4d = glm::dvec4;

	using FVector2i = glm::i32vec2;
	using FVector3i = glm::i32vec3;
	using FVector4i = glm::i32vec4;

	using FVector2 = FVector2d;
	using FVector3 = FVector3d;
	using FVector4 = FVector4d;

	using FIntPoint = FVector2i;
	using FIntVector = FVector3i;
}
