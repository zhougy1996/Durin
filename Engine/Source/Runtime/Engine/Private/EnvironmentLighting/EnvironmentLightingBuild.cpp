#include "EnvironmentLighting/EnvironmentLighting.h"

#include "Math/Operations.h"
#include "RHIResources.h"

#if defined(_MSC_VER)
#pragma optimize("t", on)
#endif

namespace Durin
{
	namespace
	{
		constexpr uint32 IrradianceSampleCount = 1024;
		constexpr uint32 BrdfSampleCount = 1024;
		constexpr float Pi = 3.14159265358979323846f;

		auto FloatToHalf(float Value) -> uint16
		{
			const uint32 Bits = std::bit_cast<uint32>(Value);
			const uint32 Sign = (Bits >> 16) & 0x8000u;
			const uint32 Mantissa = Bits & 0x007fffffu;
			const int32 Exponent = static_cast<int32>((Bits >> 23) & 0xffu) - 127 + 15;
			if (Exponent <= 0)
			{
				if (Exponent < -10) return static_cast<uint16>(Sign);
				uint32 Rounded = (Mantissa | 0x00800000u) >> (1 - Exponent);
				Rounded = (Rounded + 0x00000fffu + ((Rounded >> 13) & 1u)) >> 13;
				return static_cast<uint16>(Sign | Rounded);
			}
			if (Exponent >= 31) return static_cast<uint16>(Sign | 0x7c00u);
			const uint32 Rounded = Mantissa + 0x00000fffu + ((Mantissa >> 13) & 1u);
			if ((Rounded & 0x00800000u) != 0)
			{
				const uint32 RoundedExponent = static_cast<uint32>(Exponent + 1);
				return static_cast<uint16>(Sign | (std::min(RoundedExponent, 31u) << 10));
			}
			return static_cast<uint16>(Sign | (static_cast<uint32>(Exponent) << 10)
				| (Rounded >> 13));
		}

		auto RadicalInverse(uint32 Bits) -> float
		{
			Bits = (Bits << 16) | (Bits >> 16);
			Bits = ((Bits & 0x55555555u) << 1) | ((Bits & 0xaaaaaaaau) >> 1);
			Bits = ((Bits & 0x33333333u) << 2) | ((Bits & 0xccccccccu) >> 2);
			Bits = ((Bits & 0x0f0f0f0fu) << 4) | ((Bits & 0xf0f0f0f0u) >> 4);
			Bits = ((Bits & 0x00ff00ffu) << 8) | ((Bits & 0xff00ff00u) >> 8);
			return static_cast<float>(Bits) * 2.3283064365386963e-10f;
		}

		auto Hammersley(uint32 Index, uint32 Count) -> FVector2f
		{
			return {static_cast<float>(Index) / static_cast<float>(Count),
				RadicalInverse(Index)};
		}

		auto MakeBasisSample(const FVector3f& Normal, const FVector3f& Local) -> FVector3f
		{
			const FVector3f Up = std::abs(Normal.z) < 0.999f
				? FVector3f(0.0f, 0.0f, 1.0f)
				: FVector3f(0.0f, 1.0f, 0.0f);
			const FVector3f Tangent = Math::Normalize(Math::Cross(Up, Normal));
			const FVector3f Bitangent = Math::Cross(Normal, Tangent);
			return Math::Normalize(
				Tangent * Local.x + Bitangent * Local.y + Normal * Local.z);
		}

		auto SampleStudioRadiance(const FVector3f& Direction) -> FVector3f
		{
			const float SkyAmount = std::clamp(Direction.z * 0.5f + 0.5f, 0.0f, 1.0f);
			FVector3f Radiance = Math::Lerp(
				FVector3f(0.025f, 0.020f, 0.018f),
				FVector3f(0.18f, 0.28f, 0.50f),
				SkyAmount);
			const FVector3f KeyDirection = Math::Normalize(FVector3f(-0.4f, 0.5f, 0.75f));
			const float Key = std::pow(
				std::clamp(Math::Dot(Direction, KeyDirection), 0.0f, 1.0f), 256.0f);
			return Radiance + 6.0f * FVector3f(1.0f, 0.78f, 0.55f) * Key;
		}

		auto ImportanceSampleGgx(
			const FVector2f& Xi,
			float Roughness,
			const FVector3f& Normal) -> FVector3f
		{
			const float Alpha = Roughness * Roughness;
			const float Alpha2 = Alpha * Alpha;
			const float Phi = 2.0f * Pi * Xi.x;
			const float CosTheta = std::sqrt(
				(1.0f - Xi.y) / std::max(1.0f + (Alpha2 - 1.0f) * Xi.y, 1.0e-8f));
			const float SinTheta = std::sqrt(std::max(1.0f - CosTheta * CosTheta, 0.0f));
			return MakeBasisSample(Normal, {
				std::cos(Phi) * SinTheta,
				std::sin(Phi) * SinTheta,
				CosTheta});
		}

		auto StoreRgba16(std::vector<uint16>& Pixels, const FVector3f& Value) -> void
		{
			Pixels.push_back(FloatToHalf(Value.r));
			Pixels.push_back(FloatToHalf(Value.g));
			Pixels.push_back(FloatToHalf(Value.b));
			Pixels.push_back(FloatToHalf(1.0f));
		}

		auto BuildIrradianceFace(ETextureCubeFace Face) -> std::vector<uint16>
		{
			std::vector<uint16> Pixels;
			Pixels.reserve(EnvironmentIrradianceDimension * EnvironmentIrradianceDimension * 4);
			for (uint32 Y = 0; Y < EnvironmentIrradianceDimension; ++Y)
			{
				for (uint32 X = 0; X < EnvironmentIrradianceDimension; ++X)
				{
					FVector3 PixelDirection;
					ResolveTextureCubeFacePixelDirection(
						Face, X, Y, EnvironmentIrradianceDimension, PixelDirection);
					const FVector3f Normal = Math::Normalize(FVector3f(PixelDirection));
					FVector3f Sum(0.0f);
					for (uint32 Sample = 0; Sample < IrradianceSampleCount; ++Sample)
					{
						const FVector2f Xi = Hammersley(Sample, IrradianceSampleCount);
						const float Phi = 2.0f * Pi * Xi.x;
						const float CosTheta = std::sqrt(1.0f - Xi.y);
						const float SinTheta = std::sqrt(Xi.y);
						const FVector3f Direction = MakeBasisSample(Normal, {
							std::cos(Phi) * SinTheta,
							std::sin(Phi) * SinTheta,
							CosTheta});
						Sum += SampleStudioRadiance(Direction);
					}
					StoreRgba16(
						Pixels, Sum * (Pi / static_cast<float>(IrradianceSampleCount)));
				}
			}
			return Pixels;
		}

		auto BuildPrefilterFace(ETextureCubeFace Face, uint32 MipIndex)
			-> std::vector<uint16>
		{
			const uint32 Dimension = EnvironmentPrefilterDimension >> MipIndex;
			const uint32 SampleCount = std::max(1024u >> MipIndex, 64u);
			const float Roughness = static_cast<float>(MipIndex)
				/ static_cast<float>(EnvironmentPrefilterMipCount - 1);
			std::vector<uint16> Pixels;
			Pixels.reserve(Dimension * Dimension * 4);
			for (uint32 Y = 0; Y < Dimension; ++Y)
			{
				for (uint32 X = 0; X < Dimension; ++X)
				{
					FVector3 PixelDirection;
					ResolveTextureCubeFacePixelDirection(Face, X, Y, Dimension, PixelDirection);
					const FVector3f Normal = Math::Normalize(FVector3f(PixelDirection));
					FVector3f Sum(0.0f);
					float Weight = 0.0f;
					for (uint32 Sample = 0; Sample < SampleCount; ++Sample)
					{
						const FVector3f Half = ImportanceSampleGgx(
							Hammersley(Sample, SampleCount), Roughness, Normal);
						const FVector3f Direction = Math::Normalize(
							2.0f * Math::Dot(Normal, Half) * Half - Normal);
						const float NoL = std::max(Math::Dot(Normal, Direction), 0.0f);
						if (NoL > 0.0f)
						{
							Sum += SampleStudioRadiance(Direction) * NoL;
							Weight += NoL;
						}
					}
					StoreRgba16(
						Pixels, Weight > 0.0f ? Sum / Weight : FVector3f(0.0f));
				}
			}
			return Pixels;
		}

		auto BuildBrdfLutRows(uint32 FirstRow, uint32 EndRow) -> std::vector<uint16>
		{
			std::vector<uint16> Pixels;
			Pixels.reserve((EndRow - FirstRow) * EnvironmentBrdfLutDimension * 4);
			const FVector3f Normal(0.0f, 0.0f, 1.0f);
			for (uint32 Y = FirstRow; Y < EndRow; ++Y)
			{
				const float Roughness = (static_cast<float>(Y) + 0.5f)
					/ static_cast<float>(EnvironmentBrdfLutDimension);
				for (uint32 X = 0; X < EnvironmentBrdfLutDimension; ++X)
				{
					const float NoV = (static_cast<float>(X) + 0.5f)
						/ static_cast<float>(EnvironmentBrdfLutDimension);
					const FVector3f View(
						std::sqrt(std::max(1.0f - NoV * NoV, 0.0f)), 0.0f, NoV);
					float Scale = 0.0f;
					float Bias = 0.0f;
					const float Alpha = Roughness * Roughness;
					const float Alpha2 = Alpha * Alpha;
					for (uint32 Sample = 0; Sample < BrdfSampleCount; ++Sample)
					{
						const FVector3f Half = ImportanceSampleGgx(
							Hammersley(Sample, BrdfSampleCount), Roughness, Normal);
						const float VoH = std::max(Math::Dot(View, Half), 0.0f);
						const FVector3f Light = Math::Normalize(2.0f * VoH * Half - View);
						const float NoL = std::max(Light.z, 0.0f);
						const float NoH = std::max(Half.z, 0.0f);
						if (NoL <= 0.0f || NoH <= 0.0f || VoH <= 0.0f) continue;
						const float Visibility = 0.5f / std::max(
							NoL * std::sqrt(NoV * NoV * (1.0f - Alpha2) + Alpha2)
								+ NoV * std::sqrt(NoL * NoL * (1.0f - Alpha2) + Alpha2),
							1.0e-5f);
						const float GVis = 4.0f * Visibility * VoH * NoL / NoH;
						const float Fresnel = std::pow(1.0f - VoH, 5.0f);
						Scale += (1.0f - Fresnel) * GVis;
						Bias += Fresnel * GVis;
					}
					const float InvCount = 1.0f / static_cast<float>(BrdfSampleCount);
					Pixels.push_back(FloatToHalf(Scale * InvCount));
					Pixels.push_back(FloatToHalf(Bias * InvCount));
					Pixels.push_back(FloatToHalf(0.0f));
					Pixels.push_back(FloatToHalf(1.0f));
				}
			}
			return Pixels;
		}
	}

	auto BuildDefaultStudioEnvironmentData() -> FEnvironmentLightingData
	{
		FEnvironmentLightingData Result;
		std::array<std::future<std::vector<uint16>>, TextureCubeFaceCount>
			IrradianceTasks;
		for (uint32 Face = 0; Face < TextureCubeFaceCount; ++Face)
		{
			IrradianceTasks[Face] = std::async(std::launch::async, [Face] {
				return BuildIrradianceFace(static_cast<ETextureCubeFace>(Face));
			});
		}
		using FPrefilterFaceData =
			std::array<std::vector<uint16>, EnvironmentPrefilterMipCount>;
		std::array<std::future<FPrefilterFaceData>, TextureCubeFaceCount> PrefilterTasks;
		for (uint32 Face = 0; Face < TextureCubeFaceCount; ++Face)
		{
			PrefilterTasks[Face] = std::async(std::launch::async, [Face] {
				FPrefilterFaceData FaceData;
				for (uint32 Mip = 0; Mip < EnvironmentPrefilterMipCount; ++Mip)
				{
					FaceData[Mip] = BuildPrefilterFace(
						static_cast<ETextureCubeFace>(Face), Mip);
				}
				return FaceData;
			});
		}
		constexpr uint32 LutTaskCount = 16;
		constexpr uint32 RowsPerTask = EnvironmentBrdfLutDimension / LutTaskCount;
		std::array<std::future<std::vector<uint16>>, LutTaskCount> LutTasks;
		for (uint32 Task = 0; Task < LutTaskCount; ++Task)
		{
			LutTasks[Task] = std::async(std::launch::async, [Task] {
				return BuildBrdfLutRows(
					Task * RowsPerTask, (Task + 1) * RowsPerTask);
			});
		}
		for (uint32 Face = 0; Face < TextureCubeFaceCount; ++Face)
		{
			Result.Irradiance[Face] = IrradianceTasks[Face].get();
			FPrefilterFaceData FaceData = PrefilterTasks[Face].get();
			for (uint32 Mip = 0; Mip < EnvironmentPrefilterMipCount; ++Mip)
				Result.Prefiltered[Mip][Face] = std::move(FaceData[Mip]);
		}
		Result.BrdfLut.reserve(
			EnvironmentBrdfLutDimension * EnvironmentBrdfLutDimension * 4);
		for (auto& Task : LutTasks)
		{
			std::vector<uint16> Rows = Task.get();
			Result.BrdfLut.insert(
				Result.BrdfLut.end(),
				std::make_move_iterator(Rows.begin()),
				std::make_move_iterator(Rows.end()));
		}
		return Result;
	}
}
