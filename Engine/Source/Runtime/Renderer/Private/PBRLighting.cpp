#include "PBRLighting.h"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

namespace Durin
{
	namespace
	{
		constexpr float Pi = 3.14159265358979323846f;
		constexpr float MinVectorLengthSquared = 1.0e-8f;
		constexpr float MinBRDFDivisor = 1.0e-5f;

		auto IsFinite(const FVector3f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				&& std::isfinite(Value.z);
		}

		auto SafeNormalize(
			const FVector3f& Value,
			const FVector3f& Fallback = FVector3f(0.0f)) -> FVector3f
		{
			if (!IsFinite(Value)) return Fallback;
			const float LengthSquared = glm::dot(Value, Value);
			return LengthSquared > MinVectorLengthSquared
				? Value / std::sqrt(LengthSquared)
				: Fallback;
		}

		auto Saturate(const FVector3f& Value) -> FVector3f
		{
			return FVector3f(
				std::clamp(Value.x, 0.0f, 1.0f),
				std::clamp(Value.y, 0.0f, 1.0f),
				std::clamp(Value.z, 0.0f, 1.0f));
		}
	}

	auto EvaluatePBRDirectLighting(
		const FPBRDirectLightingInput& Input) -> FVector3f
	{
		const FVector3f BaseColor = IsFinite(Input.BaseColor)
			? Saturate(Input.BaseColor)
			: FVector3f(0.0f);
		const float Metallic = std::isfinite(Input.Metallic)
			? std::clamp(Input.Metallic, 0.0f, 1.0f)
			: 0.0f;
		const float Roughness = std::isfinite(Input.Roughness)
			? std::clamp(Input.Roughness, 0.045f, 1.0f)
			: 0.5f;
		const FVector3f Normal = SafeNormalize(
			Input.Normal, FVector3f(0.0f, 0.0f, 1.0f));
		const FVector3f ToLight = SafeNormalize(Input.ToLight);
		const FVector3f ToView = SafeNormalize(Input.ToView);
		const FVector3f HalfVector = SafeNormalize(ToLight + ToView);
		const FVector3f LightRadiance = IsFinite(Input.LightRadiance)
			? glm::max(Input.LightRadiance, FVector3f(0.0f))
			: FVector3f(0.0f);

		const float NoL = std::clamp(glm::dot(Normal, ToLight), 0.0f, 1.0f);
		const float NoV = std::clamp(glm::dot(Normal, ToView), 0.0f, 1.0f);
		const float NoH = std::clamp(glm::dot(Normal, HalfVector), 0.0f, 1.0f);
		const float VoH = std::clamp(glm::dot(ToView, HalfVector), 0.0f, 1.0f);
		const float Alpha = Roughness * Roughness;
		const float Alpha2 = Alpha * Alpha;
		const float DDenominator = std::max(
			Pi * std::pow(NoH * NoH * (Alpha2 - 1.0f) + 1.0f, 2.0f),
			MinBRDFDivisor);
		const float Distribution = Alpha2 / DDenominator;
		const float VisibilityDenominator = std::max(
			NoL * std::sqrt(NoV * NoV * (1.0f - Alpha2) + Alpha2)
				+ NoV * std::sqrt(NoL * NoL * (1.0f - Alpha2) + Alpha2),
			MinBRDFDivisor);
		const float Visibility = 0.5f / VisibilityDenominator;
		const FVector3f F0 = FVector3f(0.04f) * (1.0f - Metallic)
			+ BaseColor * Metallic;
		const float FresnelFactor = std::pow(1.0f - VoH, 5.0f);
		const FVector3f Fresnel = F0
			+ (FVector3f(1.0f) - F0) * FresnelFactor;
		const FVector3f Diffuse = (FVector3f(1.0f) - Fresnel)
			* (1.0f - Metallic) * BaseColor / Pi;
		const FVector3f Result = (Diffuse
			+ Distribution * Visibility * Fresnel) * LightRadiance * NoL;
		return IsFinite(Result) ? Result : FVector3f(0.0f);
	}

	auto EvaluatePBRMappedNormal(
		const FPBRMappedNormalInput& Input) -> FVector3f
	{
		const FVector3f FlatNormal(0.0f, 0.0f, 1.0f);
		const FVector3f GeometricNormal = SafeNormalize(
			Input.GeometricNormal, FlatNormal);
		FVector3f Tangent = IsFinite(Input.Tangent)
			? Input.Tangent - GeometricNormal
				* glm::dot(GeometricNormal, Input.Tangent)
			: FVector3f(0.0f);
		const float TangentLengthSquared = glm::dot(Tangent, Tangent);
		if (TangentLengthSquared <= MinVectorLengthSquared
			|| !std::isfinite(Input.TangentHandedness)
			|| !std::isfinite(Input.DeterminantSign)
			|| std::abs(Input.TangentHandedness * Input.DeterminantSign) <= 0.5f)
		{
			return GeometricNormal;
		}
		Tangent /= std::sqrt(TangentLengthSquared);

		FVector2f TextureXY = Input.EncodedTextureNormal * 2.0f
			- FVector2f(1.0f);
		if (!std::isfinite(TextureXY.x) || !std::isfinite(TextureXY.y))
		{
			TextureXY = FVector2f(0.0f);
		}
		const float TextureXYLengthSquared = glm::dot(TextureXY, TextureXY);
		if (TextureXYLengthSquared > 1.0f)
		{
			TextureXY /= std::sqrt(TextureXYLengthSquared);
		}
		const FVector3f TextureNormal(
			TextureXY,
			std::sqrt(std::max(
				1.0f - glm::dot(TextureXY, TextureXY), 0.0f)));
		const FVector3f ConstantNormal = SafeNormalize(
			Input.ConstantTangentNormal, FlatNormal);
		const FVector3f First = ConstantNormal + FlatNormal;
		const FVector3f Second = TextureNormal
			* FVector3f(-1.0f, -1.0f, 1.0f);
		const FVector3f TangentNormal = SafeNormalize(
			First * glm::dot(First, Second)
				/ std::max(First.z, MinBRDFDivisor) - Second,
			FlatNormal);
		const float BasisSign = Input.TangentHandedness
			* Input.DeterminantSign;
		const FVector3f Bitangent = glm::cross(GeometricNormal, Tangent)
			* BasisSign;
		return SafeNormalize(
			Tangent * TangentNormal.x + Bitangent * TangentNormal.y
				+ GeometricNormal * TangentNormal.z,
			GeometricNormal);
	}

	auto EvaluatePBREnvironmentLighting(
		const FPBREnvironmentLightingInput& Input) -> FVector3f
	{
		const FVector3f BaseColor = IsFinite(Input.BaseColor)
			? Saturate(Input.BaseColor) : FVector3f(0.0f);
		const float Metallic = std::isfinite(Input.Metallic)
			? std::clamp(Input.Metallic, 0.0f, 1.0f) : 0.0f;
		const float Roughness = std::isfinite(Input.Roughness)
			? std::clamp(Input.Roughness, 0.045f, 1.0f) : 0.5f;
		const float AmbientOcclusion = std::isfinite(Input.AmbientOcclusion)
			? std::clamp(Input.AmbientOcclusion, 0.0f, 1.0f) : 1.0f;
		const float NoV = std::isfinite(Input.NoV)
			? std::clamp(Input.NoV, 0.0f, 1.0f) : 0.0f;
		const FVector3f Irradiance = IsFinite(Input.Irradiance)
			? glm::max(Input.Irradiance, FVector3f(0.0f)) : FVector3f(0.0f);
		const FVector3f Prefiltered = IsFinite(Input.PrefilteredRadiance)
			? glm::max(Input.PrefilteredRadiance, FVector3f(0.0f)) : FVector3f(0.0f);
		const FVector2f Lut = std::isfinite(Input.BrdfLut.x)
			&& std::isfinite(Input.BrdfLut.y)
			? glm::max(Input.BrdfLut, FVector2f(0.0f)) : FVector2f(0.0f);
		const FVector3f F0 = FVector3f(0.04f) * (1.0f - Metallic)
			+ BaseColor * Metallic;
		const FVector3f Fresnel = F0
			+ (glm::max(FVector3f(1.0f - Roughness), F0) - F0)
				* std::pow(1.0f - NoV, 5.0f);
		const FVector3f Diffuse = Irradiance * BaseColor / Pi
			* (FVector3f(1.0f) - Fresnel) * (1.0f - Metallic);
		const FVector3f Specular = Prefiltered * (F0 * Lut.x + Lut.y);
		const FVector3f Result = (Diffuse + Specular) * AmbientOcclusion;
		return IsFinite(Result) ? Result : FVector3f(0.0f);
	}
}
