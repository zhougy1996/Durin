#pragma once

#include "Math/Vector.h"
#include "RendererAPI.h"

namespace Durin
{
	struct FPBRDirectLightingInput
	{
		FVector3f BaseColor{1.0f};
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		FVector3f Normal{0.0f, 0.0f, 1.0f};
		FVector3f ToLight{0.0f, 0.0f, 1.0f};
		FVector3f ToView{0.0f, 0.0f, 1.0f};
		FVector3f LightRadiance{1.0f};
	};

	struct FPBRMappedNormalInput
	{
		FVector3f ConstantTangentNormal{0.0f, 0.0f, 1.0f};
		FVector2f EncodedTextureNormal{0.5f, 0.5f};
		FVector3f GeometricNormal{0.0f, 0.0f, 1.0f};
		FVector3f Tangent{1.0f, 0.0f, 0.0f};
		float TangentHandedness = 1.0f;
		float DeterminantSign = 1.0f;
	};

	struct FPBREnvironmentLightingInput
	{
		FVector3f BaseColor{1.0f};
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		float AmbientOcclusion = 1.0f;
		float NoV = 1.0f;
		FVector3f Irradiance{0.0f};
		FVector3f PrefilteredRadiance{0.0f};
		FVector2f BrdfLut{0.0f};
	};

	// CPU reference for the direct-light shader contract. Inputs are
	// canonicalized again so degenerate vectors and out-of-range surface values
	// always produce finite deterministic output.
	RENDERER_API auto EvaluatePBRDirectLighting(
		const FPBRDirectLightingInput& Input) -> FVector3f;

	// CPU reference for the bounded local-light inverse-square and range window.
	RENDERER_API auto EvaluatePointLightAttenuation(
		float DistanceSquared, float Range) -> float;

	// CPU reference for smooth or equal-angle hard-edged spot attenuation.
	RENDERER_API auto EvaluateSpotLightConeAttenuation(
		float AngleCosine, float InnerCosine, float OuterCosine) -> float;

	// CPU reference for normal-texture decode, RNM composition, and mirrored
	// tangent-basis reconstruction used by the StaticMesh shader.
	RENDERER_API auto EvaluatePBRMappedNormal(
		const FPBRMappedNormalInput& Input) -> FVector3f;

	// CPU reference for split-sum environment composition and its AO scope.
	RENDERER_API auto EvaluatePBREnvironmentLighting(
		const FPBREnvironmentLightingInput& Input) -> FVector3f;
}
