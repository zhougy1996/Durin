#pragma once

#include "Math/DurinMath.h"
#include "RendererAPI.h"

namespace Durin::GBufferContract
{
	inline constexpr uint8 StandardLitFlag = 1u;
	inline constexpr float MaximumUNorm8Error = 1.0f / 510.0f;
	inline constexpr double MaximumNormalErrorDegrees = 1.0;

	struct FDecodedRecord
	{
		FVector3f BaseColor{0.0f};
		FVector3f ShadingNormal{0.0f, 0.0f, 1.0f};
		FVector3f GeometricNormal{0.0f, 0.0f, 1.0f};
		FVector3f Emissive{0.0f};
		float Metallic = 0.0f;
		float Roughness = 0.0f;
		float AmbientOcclusion = 0.0f;
		float EffectiveOpacity = 0.0f;
		uint8 Flags = 0;

		auto IsStandardLit() const -> bool
		{
			return (Flags & StandardLitFlag) != 0;
		}
	};

	RENDERER_API auto EncodeOctahedralNormal(const FVector3f& Normal)
		-> FVector2f;
	RENDERER_API auto DecodeOctahedralNormal(const FVector2f& Encoded)
		-> FVector3f;
	RENDERER_API auto DecodeRecord(
		const FVector4f& Material,
		const FVector4f& Normals,
		const FVector4f& Surface,
		const FVector3f& Emissive) -> FDecodedRecord;
	RENDERER_API auto DecodeR11G11B10Float(uint32 Packed) -> FVector3f;

	// Solves the three homogeneous projection equations directly in view space.
	// This avoids an inverse view-projection multiply and therefore never mixes
	// large world translations into the depth reconstruction.
	RENDERER_API auto ReconstructViewPositionAnalytic(
		const FMatrix& Projection,
		const FVector2f& Ndc,
		double DeviceDepth,
		FVector3& OutViewPosition) -> bool;

	RENDERER_API auto GetPositionTolerance(double DistanceToView) -> double;
} // namespace Durin::GBufferContract
