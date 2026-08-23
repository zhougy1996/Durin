#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "GBufferContract.h"
#include "HAL/PlatformLTS.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialRenderProxy.h"
#include "Math/Operations.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"
#include "RHICommandList.h"
#include "RendererModule.h"
#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/SceneVisibility.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "StaticMesh/StaticMeshResources.h"

#include <gtest/gtest.h>

#include <array>
#include <complex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	constexpr uint32 CaptureWidth = 257;
	constexpr uint32 CaptureHeight = 257;

	struct FShadowBaselineCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ShadowBaselineCapture";
		}
	};

	struct FCaptureStatistics
	{
		std::string Name;
		std::string Hash;
		size_t DarkPixels = 0;
		size_t MidPixels = 0;
		size_t BrightPixels = 0;
		std::array<uint8, 4> Minimum{255, 255, 255, 255};
		std::array<uint8, 4> Maximum{0, 0, 0, 0};
		std::array<double, 4> Mean{};
		Durin::FViewRenderCounters Counters;
	};

	struct FPrimitivePlacement
	{
		Durin::FVector3 Translation{0.0};
		Durin::FVector3 Scale{1.0};
		double RotationYDegrees = 0.0;
		double RotationZDegrees = 0.0;
		bool bMasked = false;
	};

	struct FFixture
	{
		std::string Name;
		std::vector<FPrimitivePlacement> Primitives;
		Durin::FVector3 LightDirection{0.35, 0.2, -1.0};
		double CameraTranslationX = 0.0;
		bool bCastShadows = true;
		Durin::EDirectionalShadowDiagnosticMode DiagnosticMode =
			Durin::EDirectionalShadowDiagnosticMode::Lit;
		Durin::ERenderMode RenderMode = Durin::ERenderMode::Lit;
		Durin::EDirectionalShadowFilterQuality FilterQuality =
			Durin::EDirectionalShadowFilterQuality::Low;
		Durin::EDirectionalShadowCandidate Candidate =
			Durin::EDirectionalShadowCandidate::SingleMap;
		bool bPerspective = false;
	};

	Durin::FViewRenderCounters GLastCounters;
	std::vector<std::byte>* GHDRSceneColorPixels = nullptr;
	std::vector<std::byte>* GHDRPostProcessInputPixels = nullptr;
	std::vector<std::byte>* GGBufferMaterialPixels = nullptr;
	std::vector<std::byte>* GGBufferNormalsPixels = nullptr;
	std::vector<std::byte>* GGBufferSurfacePixels = nullptr;
	std::vector<std::byte>* GGBufferEmissivePixels = nullptr;
	std::vector<std::byte>* GDeferredDirectionalPixels = nullptr;

	auto CaptureCounters(const Durin::FViewRenderCounters& Counters) -> void
	{
		GLastCounters = Counters;
	}

	auto CaptureHDRSceneColor(
		Durin::FRHICommandListImmediate& CommandList,
		Durin::FRHITexture* SceneColor,
		Durin::FRHITexture* PostProcessInput
	) -> void
	{
		auto Capture = [&CommandList](
						   const char* Name,
						   Durin::FRHITexture* Source,
						   std::vector<std::byte>* Pixels
					   ) {
			if (Pixels == nullptr) return;
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  Name,
								  Source->GetSizeX(),
								  Source->GetSizeY(),
								  Source->GetFormat()
			)
								  .SetFlags(Durin::ETextureCreateFlags::DestinationCopy | Durin::ETextureCreateFlags::CPUReadback | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Readback =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Readback, nullptr);
			const Durin::FRHITextureSubresourceRange WholeColor{
				Durin::ERHITextureAspect::Color, 0, 1, 0, 1
			};
			CommandList.TransitionTextures(std::array{
				Durin::FRHITextureTransition{Source, WholeColor, Durin::ERHIAccess::GraphicsShaderRead, Durin::ERHIAccess::TransferRead},
				Durin::FRHITextureTransition{Readback, WholeColor, Durin::ERHIAccess::Discard, Durin::ERHIAccess::TransferWrite}
			});
			CommandList.CopyTexture(Source, Readback, std::array{Durin::FRHITextureCopyRegion{.Extent = {Source->GetSizeX(), Source->GetSizeY(), 1}}});
			CommandList.TransitionTextures(std::array{
				Durin::FRHITextureTransition{Source, WholeColor, Durin::ERHIAccess::TransferRead, Durin::ERHIAccess::GraphicsShaderRead},
				Durin::FRHITextureTransition{Readback, WholeColor, Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead}
			});
			EXPECT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Readback, 0, 0, *Pixels
			));
		};
		Capture("HDRSceneColorReadback", SceneColor, GHDRSceneColorPixels);
		Capture("HDRPostProcessInputReadback", PostProcessInput, GHDRPostProcessInputPixels);
	}

	auto CaptureGBuffer(
		Durin::FRHICommandListImmediate& CommandList,
		Durin::FRHITexture* Material,
		Durin::FRHITexture* Normals,
		Durin::FRHITexture* Surface,
		Durin::FRHITexture* Emissive,
		Durin::FRHITexture*
	) -> void
	{
		auto Capture = [&CommandList](
						   const char* Name,
						   Durin::FRHITexture* Source,
						   std::vector<std::byte>* Pixels,
						   Durin::ERHITextureAspect Aspect =
							   Durin::ERHITextureAspect::Color
					   ) {
			if (Pixels == nullptr) return;
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  Name, Source->GetSizeX(), Source->GetSizeY(), Source->GetFormat()
			)
								  .SetFlags(Durin::ETextureCreateFlags::DestinationCopy | Durin::ETextureCreateFlags::CPUReadback | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Readback =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Readback, nullptr);
			const Durin::FRHITextureSubresourceRange Whole{
				Aspect, 0, 1, 0, 1
			};
			CommandList.TransitionTextures(std::array{
				Durin::FRHITextureTransition{Source, Whole, Durin::ERHIAccess::GraphicsShaderRead, Durin::ERHIAccess::TransferRead},
				Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::Discard, Durin::ERHIAccess::TransferWrite}
			});
			CommandList.CopyTexture(Source, Readback, std::array{Durin::FRHITextureCopyRegion{.SourceAspect = Aspect, .DestinationAspect = Aspect, .Extent = {Source->GetSizeX(), Source->GetSizeY(), 1}}});
			CommandList.TransitionTextures(std::array{
				Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead},
				Durin::FRHITextureTransition{Source, Whole, Durin::ERHIAccess::TransferRead, Durin::ERHIAccess::GraphicsShaderRead}
			});
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Readback, 0, 0, *Pixels
			));
		};
		Capture("GBufferMaterialReadback", Material, GGBufferMaterialPixels);
		Capture("GBufferNormalsReadback", Normals, GGBufferNormalsPixels);
		Capture("GBufferSurfaceReadback", Surface, GGBufferSurfacePixels);
		Capture("GBufferEmissiveReadback", Emissive, GGBufferEmissivePixels);
	}

	auto CaptureDeferredDirectional(
		Durin::FRHICommandListImmediate& CommandList,
		Durin::FRHITexture* DeferredColor
	) -> void
	{
		if (GDeferredDirectionalPixels == nullptr) return;
		const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
							  "DeferredDirectionalReadback",
							  DeferredColor->GetSizeX(), DeferredColor->GetSizeY(),
							  DeferredColor->GetFormat()
		)
							  .SetFlags(Durin::ETextureCreateFlags::DestinationCopy | Durin::ETextureCreateFlags::CPUReadback | Durin::ETextureCreateFlags::ShaderResource);
		Durin::FTextureRHIRef Readback =
			Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
		ASSERT_NE(Readback, nullptr);
		const Durin::FRHITextureSubresourceRange Whole{
			Durin::ERHITextureAspect::Color, 0, 1, 0, 1
		};
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{DeferredColor, Whole, Durin::ERHIAccess::GraphicsShaderRead, Durin::ERHIAccess::TransferRead},
			Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::Discard, Durin::ERHIAccess::TransferWrite}
		});
		CommandList.CopyTexture(DeferredColor, Readback, std::array{Durin::FRHITextureCopyRegion{.Extent = {DeferredColor->GetSizeX(), DeferredColor->GetSizeY(), 1}}});
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead},
			Durin::FRHITextureTransition{DeferredColor, Whole, Durin::ERHIAccess::TransferRead, Durin::ERHIAccess::GraphicsShaderRead}
		});
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			CommandList, Readback, 0, 0, *GDeferredDirectionalPixels
		));
	}

	auto MakeQuadRenderData() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FStaticMeshRenderData>();
		Data->MaterialSlots = {{"Surface", 0}};
		auto& LOD = Data->LODResources.emplace_back();
		const std::vector<Durin::FVector3f> Positions{
			{-1.0f, -1.0f, 0.0f},
			{1.0f, -1.0f, 0.0f},
			{1.0f, 1.0f, 0.0f},
			{-1.0f, 1.0f, 0.0f}
		};
		LOD.VertexBuffers.PositionVertexBuffer.Init(Positions);
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(4, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(4, {1.0f, 0.0f, 0.0f, 1.0f})
		);
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels>
			UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), 4, 1
		);
		LOD.VertexBuffers.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(4, Durin::FVector4f(1.0f)), 4
		);
		LOD.IndexBuffer.Init({0, 1, 2, 0, 2, 3});
		LOD.Sections.push_back({.Name = "Surface", .FirstIndex = 0, .IndexCount = 6, .MinVertexIndex = 0, .MaxVertexIndex = 3, .MaterialSlotIndex = 0, .LocalBounds = Durin::FBox({-1.0, -1.0, 0.0}, {1.0, 1.0, 0.0})});
		LOD.LocalBounds = LOD.Sections[0].LocalBounds;
		LOD.NumTexCoords = 1;
		LOD.bHasColorVertexData = true;
		Data->LODVertexFactories.resize(1);
		Data->RecalculateBounds();
		return Data;
	}

	auto MakeMaterial(Durin::EMaterialBlendMode BlendMode, const Durin::FVector3& BaseColor, Durin::EMaterialShadingModel ShadingModel = Durin::EMaterialShadingModel::Lit, const Durin::FVector3& Emissive = Durin::FVector3(0.0), float Opacity = 1.0f)
		-> Durin::FMaterialRenderProxyRef
	{
		auto Proxy = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
		Durin::FMaterialRenderProxyPublication Publication;
		Publication.LocalVersion = 1;
		Publication.LocalLayer.StaticProperties = Durin::FMaterialStaticProperties{
			.BlendMode = BlendMode,
			.ShadingModel = ShadingModel,
			.bTwoSided = true,
			.OpacityMaskThreshold = 0.4f
		};
		Publication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::EmissiveId, .Type = Durin::EMaterialParameterType::Vector, .VectorValue = Emissive});
		Publication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::BaseColorId, .Type = Durin::EMaterialParameterType::Vector, .VectorValue = BaseColor});
		if (BlendMode == Durin::EMaterialBlendMode::Translucent)
		{
			Publication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::OpacityId, .Type = Durin::EMaterialParameterType::Scalar, .ScalarValue = Opacity});
		}
		if (BlendMode == Durin::EMaterialBlendMode::Masked)
		{
			Publication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::OpacityMaskId, .Type = Durin::EMaterialParameterType::Scalar, .ScalarValue = 1.0f});
		}
		EXPECT_TRUE(Proxy->QueuePublication_GameThread(std::move(Publication)));
		return Proxy;
	}

	auto MakeTransform(const FPrimitivePlacement& Placement) -> Durin::FMatrix
	{
		Durin::FMatrix Transform = Durin::Math::TranslationMatrix(Placement.Translation);
		if (Placement.RotationYDegrees != 0.0)
		{
			Transform = Durin::Math::RotateDegrees(
				Transform, Placement.RotationYDegrees, Durin::FVectorConstants::Right);
		}
		if (Placement.RotationZDegrees != 0.0)
		{
			Transform = Durin::Math::RotateDegrees(
				Transform, Placement.RotationZDegrees, Durin::FVectorConstants::Up);
		}
		return Durin::Math::Scale(Transform, Placement.Scale);
	}

	auto CalculateStatistics(std::string Name, const std::vector<std::byte>& Pixels, const Durin::FViewRenderCounters& Counters) -> FCaptureStatistics
	{
		FCaptureStatistics Result;
		Result.Name = std::move(Name);
		Result.Hash = Durin::FXxHash128::HashBuffer(Pixels).ToString();
		Result.Counters = Counters;
		for (size_t Offset = 0; Offset + 3 < Pixels.size(); Offset += 4)
		{
			const unsigned Luminance =
				(static_cast<unsigned>(Pixels[Offset]) * 54u
				 + static_cast<unsigned>(Pixels[Offset + 1]) * 183u
				 + static_cast<unsigned>(Pixels[Offset + 2]) * 19u)
				/ 256u;
			Result.DarkPixels += Luminance < 48u ? 1u : 0u;
			Result.MidPixels += Luminance >= 48u && Luminance < 160u ? 1u : 0u;
			Result.BrightPixels += Luminance >= 160u ? 1u : 0u;
			for (size_t Channel = 0; Channel < 4; ++Channel)
			{
				Result.Minimum[Channel] = std::min(
					Result.Minimum[Channel], Pixels[Offset + Channel]
				);
				Result.Maximum[Channel] = std::max(
					Result.Maximum[Channel], Pixels[Offset + Channel]
				);
				Result.Mean[Channel] += Pixels[Offset + Channel];
			}
		}
		const double PixelCount = static_cast<double>(Pixels.size() / 4u);
		for (double& Value : Result.Mean)
			Value /= PixelCount;
		return Result;
	}

	auto CountChangedPixels(const std::vector<std::byte>& First, const std::vector<std::byte>& Second, uint8 ChannelTolerance)
		-> size_t
	{
		if (First.size() != Second.size()) return SIZE_MAX;
		size_t Changed = 0;
		for (size_t Offset = 0; Offset + 3 < First.size(); Offset += 4)
		{
			bool bChanged = false;
			for (size_t Channel = 0; Channel < 3; ++Channel)
			{
				const int Difference = static_cast<int>(First[Offset + Channel])
									   - static_cast<int>(Second[Offset + Channel]);
				bChanged |= std::abs(Difference) > ChannelTolerance;
			}
			Changed += bChanged ? 1u : 0u;
		}
		return Changed;
	}

	auto LuminanceAt(const std::vector<std::byte>& Pixels, uint32 X, uint32 Y) -> int
	{
		const size_t Offset = (static_cast<size_t>(Y) * CaptureWidth + X) * 4u;
		return static_cast<int>((static_cast<unsigned>(Pixels[Offset]) * 54u + static_cast<unsigned>(Pixels[Offset + 1]) * 183u + static_cast<unsigned>(Pixels[Offset + 2]) * 19u) / 256u);
	}

	auto MaximumTransitionWidth(
		const std::vector<std::byte>& Pixels
	) -> size_t
	{
		size_t Maximum = 0;
		auto MeasureLine = [&](bool bVertical, uint32 Fixed) {
			size_t Run = 0;
			for (uint32 Variable = 0; Variable < CaptureWidth; ++Variable)
			{
				const uint32 X = bVertical ? Fixed : Variable;
				const uint32 Y = bVertical ? Variable : Fixed;
				const int Luminance = LuminanceAt(Pixels, X, Y);
				Run = Luminance >= 48 && Luminance < 160 ? Run + 1u : 0u;
				Maximum = std::max(Maximum, Run);
			}
		};
		for (uint32 Line = 0; Line < CaptureWidth; ++Line)
		{
			MeasureLine(false, Line);
			MeasureLine(true, Line);
		}
		return Maximum;
	}

	auto ShadowDifferenceAt(
		const std::vector<std::byte>& Enabled,
		const std::vector<std::byte>& Disabled,
		uint32 X,
		uint32 Y
	) -> int
	{
		const int Reference = LuminanceAt(Disabled, X, Y);
		return Reference >= 160 ? std::max(Reference - LuminanceAt(Enabled, X, Y), 0) : 0;
	}

	auto TransformRadix2(std::complex<double>* Values, size_t Count) -> void
	{
		for (size_t Index = 1, Reversed = 0; Index < Count; ++Index)
		{
			size_t Bit = Count >> 1u;
			for (; (Reversed & Bit) != 0; Bit >>= 1u)
				Reversed ^= Bit;
			Reversed ^= Bit;
			if (Index < Reversed) std::swap(Values[Index], Values[Reversed]);
		}
		for (size_t Length = 2; Length <= Count; Length <<= 1u)
		{
			const double Angle = -2.0 * 3.14159265358979323846
								 / static_cast<double>(Length);
			const std::complex<double> Root(std::cos(Angle), std::sin(Angle));
			for (size_t Start = 0; Start < Count; Start += Length)
			{
				std::complex<double> Weight(1.0, 0.0);
				for (size_t Offset = 0; Offset < Length / 2u; ++Offset)
				{
					const std::complex<double> Even = Values[Start + Offset];
					const std::complex<double> Odd =
						Values[Start + Offset + Length / 2u] * Weight;
					Values[Start + Offset] = Even + Odd;
					Values[Start + Offset + Length / 2u] = Even - Odd;
					Weight *= Root;
				}
			}
		}
	}

	auto CalculateShadowOnlyHighFrequencyFraction(
		const std::vector<std::byte>& Enabled,
		const std::vector<std::byte>& Disabled
	) -> double
	{
		constexpr size_t TransformSize = 256;
		constexpr size_t RoiMinimum = 32;
		constexpr size_t RoiMaximum = 225;
		constexpr size_t RoiSize = RoiMaximum - RoiMinimum;
		std::vector<std::complex<double>> Values(
			TransformSize * TransformSize
		);
		double Mean = 0.0;
		for (size_t Y = RoiMinimum; Y < RoiMaximum; ++Y)
			for (size_t X = RoiMinimum; X < RoiMaximum; ++X)
				Mean += ShadowDifferenceAt(Enabled, Disabled, static_cast<uint32>(X), static_cast<uint32>(Y));
		Mean /= static_cast<double>(RoiSize * RoiSize);
		for (size_t Y = 0; Y < RoiSize; ++Y)
			for (size_t X = 0; X < RoiSize; ++X)
				Values[Y * TransformSize + X] =
					static_cast<double>(ShadowDifferenceAt(Enabled, Disabled, static_cast<uint32>(X + RoiMinimum), static_cast<uint32>(Y + RoiMinimum))) - Mean;
		for (size_t Y = 0; Y < TransformSize; ++Y)
			TransformRadix2(Values.data() + Y * TransformSize, TransformSize);
		std::array<std::complex<double>, TransformSize> Column;
		for (size_t X = 0; X < TransformSize; ++X)
		{
			for (size_t Y = 0; Y < TransformSize; ++Y)
				Column[Y] = Values[Y * TransformSize + X];
			TransformRadix2(Column.data(), TransformSize);
			for (size_t Y = 0; Y < TransformSize; ++Y)
				Values[Y * TransformSize + X] = Column[Y];
		}
		double TotalEnergy = 0.0;
		double HighFrequencyEnergy = 0.0;
		for (size_t Y = 0; Y < TransformSize; ++Y)
			for (size_t X = 0; X < TransformSize; ++X)
			{
				const double FrequencyX = static_cast<double>(
					X <= TransformSize / 2u ? X : TransformSize - X
				);
				const double FrequencyY = static_cast<double>(
					Y <= TransformSize / 2u ? Y : TransformSize - Y
				);
				const double Energy = std::norm(Values[Y * TransformSize + X]);
				TotalEnergy += Energy;
				if (std::sqrt(FrequencyX * FrequencyX + FrequencyY * FrequencyY)
						/ static_cast<double>(TransformSize)
					> 0.30)
					HighFrequencyEnergy += Energy;
			}
		return TotalEnergy > 0.0 ? HighFrequencyEnergy / TotalEnergy : 0.0;
	}


	auto WritePpm(const std::filesystem::path& Path, const std::vector<std::byte>& Pixels) -> void
	{
		std::ofstream Stream(Path, std::ios::binary);
		ASSERT_TRUE(Stream.is_open()) << Path.string();
		Stream << "P6\n"
			   << CaptureWidth << ' ' << CaptureHeight << "\n255\n";
		for (size_t Offset = 0; Offset + 3 < Pixels.size(); Offset += 4)
		{
			Stream.write(reinterpret_cast<const char*>(Pixels.data() + Offset), 3);
		}
		ASSERT_TRUE(Stream.good()) << Path.string();
	}

	auto WriteMetrics(const std::filesystem::path& Path, const std::vector<FCaptureStatistics>& Statistics, const std::array<size_t, 3>& MotionChangedPixels, const std::array<size_t, 2>& Q1EntryMotionChangedPixels, const std::array<double, 3>& ShadowOnlyHighFrequencyFraction, const std::array<size_t, 2>& MediumMotion, const std::array<size_t, 2>& HighMotion) -> void
	{
		std::ofstream Stream(Path);
		ASSERT_TRUE(Stream.is_open()) << Path.string();
		Stream << "{\n  \"schema\": 1,\n  \"captures\": [\n";
		for (size_t Index = 0; Index < Statistics.size(); ++Index)
		{
			const FCaptureStatistics& Value = Statistics[Index];
			Stream << "    {\"name\": \"" << Value.Name << "\", \"xxh128\": \""
				   << Value.Hash << "\", \"dark\": " << Value.DarkPixels
				   << ", \"mid\": " << Value.MidPixels << ", \"bright\": "
				   << Value.BrightPixels << ", \"mean\": [" << std::fixed
				   << std::setprecision(3) << Value.Mean[0] << ", " << Value.Mean[1]
				   << ", " << Value.Mean[2] << ", " << Value.Mean[3]
				   << "], \"shadow\": {\"selectedLights\": "
				   << Value.Counters.ShadowSelectedLights << ", \"validViews\": "
				   << Value.Counters.ShadowValidReceiverViews << ", \"draws\": "
				   << Value.Counters.ShadowSuccessfulDraws << "}}"
				   << (Index + 1 == Statistics.size() ? "\n" : ",\n");
		}
		Stream << "  ],\n  \"motionChangedPixelsAtTolerance2\": ["
			   << MotionChangedPixels[0] << ", " << MotionChangedPixels[1] << ", "
			   << MotionChangedPixels[2] << "],\n"
			   << "  \"q1EntryMotionChangedPixelsAtTolerance2\": ["
			   << Q1EntryMotionChangedPixels[0] << ", "
			   << Q1EntryMotionChangedPixels[1] << "],\n"
			   << "  \"shadowOnlyHighFrequencyFraction\": ["
			   << std::setprecision(9) << ShadowOnlyHighFrequencyFraction[0] << ", "
			   << ShadowOnlyHighFrequencyFraction[1] << ", "
			   << ShadowOnlyHighFrequencyFraction[2] << "],\n"
			   << "  \"mediumMotionChangedPixelsAtTolerance2\": ["
			   << MediumMotion[0] << ", " << MediumMotion[1] << "],\n"
			   << "  \"highMotionChangedPixelsAtTolerance2\": ["
			   << HighMotion[0] << ", " << HighMotion[1] << "]\n}\n";
		ASSERT_TRUE(Stream.good()) << Path.string();
	}
} // namespace

TEST(FDirectionalShadowBaselineVulkanTests, CapturesFrozenLitArtifactsAndSubTexelMotion)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::FRHIInitializationContext::Headless());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	Durin::FRendererModule Renderer;
	Durin::FModuleTestHarness RendererLifecycle("DirectionalShadowRendererTest");
	RendererLifecycle.Start(Renderer);
	Durin::SetViewRenderCounterSink(CaptureCounters);

	auto Quad = MakeQuadRenderData();
	Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(Quad->InitResources(CommandList));
		}
	);
	Durin::FlushRenderingCommands();
	auto Opaque = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque, {0.72, 0.72, 0.72}
	);
	auto Masked = MakeMaterial(
		Durin::EMaterialBlendMode::Masked, {0.72, 0.72, 0.72}
	);

	std::vector<FFixture> Fixtures{
		{"q0_planar_acne_valid",
		 {{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.82, 0.82, 1.0}}}},
		{"q0_sloped_acne_valid",
		 {{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.75, 0.75, 1.0}, .RotationYDegrees = 28.0}}},
		{"q0_contact_detachment_valid",
		 {{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.82, 0.82, 1.0}},
		  {.Translation = {-0.18, 0.08, 0.52}, .Scale = {0.22, 0.18, 1.0}}}},
		{"q0_modular_seam_valid",
		 {{.Translation = {-0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}}},
		{"q0_modular_gap_defective",
		 {{.Translation = {-0.42, 0.0, 0.4}, .Scale = {0.38, 0.8, 1.0}},
		  {.Translation = {0.42, 0.0, 0.4}, .Scale = {0.38, 0.8, 1.0}},
		  {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}}},
		{"q0_grazing_angle_valid",
		 {{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.82, 0.82, 1.0}},
		  {.Translation = {-0.18, 0.08, 0.65}, .Scale = {0.22, 0.18, 1.0}}},
		 {0.995, 0.0, -0.1}},
		{"q0_masked_coverage_valid",
		 {{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.82, 0.82, 1.0}},
		  {.Translation = {-0.18, 0.08, 0.65}, .Scale = {0.22, 0.18, 1.0}, .bMasked = true}}},
		{"q0_opaque_coverage_reference",
		 {{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.82, 0.82, 1.0}},
		  {.Translation = {-0.18, 0.08, 0.65}, .Scale = {0.22, 0.18, 1.0}}}},
		{"q0_shadow_disabled_reference",
		 {{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.82, 0.82, 1.0}},
		  {.Translation = {-0.18, 0.08, 0.52}, .Scale = {0.22, 0.18, 1.0}}},
		 {0.35, 0.2, -1.0},
		 0.0,
		 false},
		{"q0_motion_translate_00",
		 {{.Translation = {-0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}}},
		{"q0_motion_translate_01",
		 {{.Translation = {-0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}},
		 {0.35, 0.2, -1.0},
		 0.000244140625},
		{"q0_motion_translate_02",
		 {{.Translation = {-0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}},
		 {0.35, 0.2, -1.0},
		 0.00048828125},
		{"q0_motion_light_01",
		 {{.Translation = {-0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
		  {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}},
		 {0.3505, 0.2, -1.0}},
		{.Name = "q0_diagnostic_depth_coverage",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::ShadowDepthCoverage},
		{.Name = "q0_diagnostic_receiver_unbiased",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::ReceiverUnbiased},
		{.Name = "q0_diagnostic_receiver_biased",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::ReceiverBiased},
		{.Name = "q0_diagnostic_normal_offset",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::ReceiverNormalOffset},
		{.Name = "q0_diagnostic_texel_grid",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::TexelGrid},
		{.Name = "q0_diagnostic_bias_contributions",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::BiasContributions},
		{.Name = "q0_diagnostic_classification",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::Classification},
		{.Name = "q0_diagnostic_disabled_reference",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .bCastShadows = false,
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::Classification},
		{.Name = "q0_unlit_reference",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .RenderMode = Durin::ERenderMode::Unlit},
		{.Name = "q0_diagnostic_unlit_reference",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}}, {{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::Classification,
		 .RenderMode = Durin::ERenderMode::Unlit},
		// Q1 entry captures stay after every Q0 index to preserve baseline identity.
		{.Name = "q1_edge_staircase_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}}},
		{.Name = "q1_diagonal_silhouette_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{-0.08, 0.04, 0.64}, {0.46, 0.22, 1.0}, 0.0, 43.0}}},
		{.Name = "q1_thin_caster_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{-0.12, 0.02, 0.64}, {0.018, 0.62, 1.0}, 0.0, 11.0}}},
		{.Name = "q1_masked_cutout_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{-0.28, 0.0, 0.64}, {0.12, 0.55, 1.0}, 0.0, 0.0, true}, {{0.28, 0.0, 0.64}, {0.12, 0.55, 1.0}, 0.0, 0.0, true}, {{0.0, 0.36, 0.64}, {0.16, 0.10, 1.0}, 0.0, 0.0, true}, {{0.0, -0.36, 0.64}, {0.16, 0.10, 1.0}, 0.0, 0.0, true}}},
		{.Name = "q1_guard_boundary_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.98, 0.9, 1.0}}, {{0.91, 0.0, 0.64}, {0.055, 0.55, 1.0}, 0.0, 7.0}}},
		{.Name = "q1_motion_subpixel_00_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}}},
		{.Name = "q1_motion_subpixel_01_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .CameraTranslationX = 0.0001220703125},
		{.Name = "q1_motion_light_01_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .LightDirection = {0.35025, 0.2, -1.0}},
		{.Name = "q1_edge_staircase_shadow_disabled",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .bCastShadows = false},
		{.Name = "q1_edge_staircase_medium",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium},
		{.Name = "q1_motion_subpixel_00_medium",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium},
		{.Name = "q1_motion_subpixel_01_medium",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .CameraTranslationX = 0.0001220703125,
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium},
		{.Name = "q1_motion_light_01_medium",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .LightDirection = {0.35025, 0.2, -1.0},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium},
		{.Name = "q1_edge_staircase_high",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::High},
		{.Name = "q1_motion_subpixel_00_high",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::High},
		{.Name = "q1_motion_subpixel_01_high",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .CameraTranslationX = 0.0001220703125,
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::High},
		{.Name = "q1_motion_light_01_high",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}}, {{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .LightDirection = {0.35025, 0.2, -1.0},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::High}
	};
	constexpr size_t LowFixtureCount = 31u;
	constexpr std::array<size_t, 21> ParityFixtureIndices{
		0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u,
		23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u
	};
	auto AppendTierParity = [&](Durin::EDirectionalShadowFilterQuality Quality,
								std::string_view Suffix) {
		for (const size_t SourceIndex : ParityFixtureIndices)
		{
			FFixture Copy = Fixtures[SourceIndex];
			if (Copy.Name.ends_with("_low"))
				Copy.Name.erase(Copy.Name.size() - 4u);
			Copy.Name += Suffix;
			Copy.FilterQuality = Quality;
			Fixtures.push_back(std::move(Copy));
		}
	};
	const size_t MediumParityStart = Fixtures.size();
	AppendTierParity(
		Durin::EDirectionalShadowFilterQuality::Medium, "_medium"
	);
	const size_t HighParityStart = Fixtures.size();
	AppendTierParity(
		Durin::EDirectionalShadowFilterQuality::High, "_high"
	);
	const size_t FilterDiagnosticStart = Fixtures.size();
	for (const auto [Quality, Suffix] : std::array{
			 std::pair{Durin::EDirectionalShadowFilterQuality::Medium, std::string_view{"medium"}},
			 std::pair{Durin::EDirectionalShadowFilterQuality::High, std::string_view{"high"}}
		 })
	{
		for (const auto [Mode, ModeName] : std::array{
				 std::pair{Durin::EDirectionalShadowDiagnosticMode::FilterFootprint, std::string_view{"footprint"}},
				 std::pair{Durin::EDirectionalShadowDiagnosticMode::FilterTapValidity, std::string_view{"tap_validity"}},
				 std::pair{Durin::EDirectionalShadowDiagnosticMode::FilterDifference, std::string_view{"difference"}}
			 })
		{
			FFixture Diagnostic = Fixtures[27u];
			Diagnostic.Name = std::format(
				"q1_diagnostic_{}_{}", ModeName, Suffix
			);
			Diagnostic.DiagnosticMode = Mode;
			Diagnostic.FilterQuality = Quality;
			Fixtures.push_back(std::move(Diagnostic));
		}
	}
	const size_t CascadeFixtureStart = Fixtures.size();
	Fixtures.push_back({.Name = "q2_cascades_index_perspective", .Primitives = {{{24.0, 0.0, 0.0}, {7.0, 7.0, 1.0}, 90.0}, {{22.0, 0.5, 0.0}, {2.0, 2.0, 1.0}, 90.0}}, .LightDirection = {-1.0, 0.2, -0.25}, .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::CascadeIndex, .FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium, .Candidate = Durin::EDirectionalShadowCandidate::ThreeCascades, .bPerspective = true});
	constexpr std::array<std::string_view, 13> ExpectedHashes{
		"fe949c60fa7314c9154b390d9570b60a",
		"148649a1ec8638f27c071034a1379afd",
		"12f14c97e2a78de7a16bfb84ad1fee4f",
		"0e23e4f5fb83a4802011082773a04a48",
		"814082869560a86c7a49a071f4a9bbcb",
		"bf00b0661b21f4b5e6a780680f66be7c",
		"6fc3ad3c1350a8ec0457611a2975a171",
		"6fc3ad3c1350a8ec0457611a2975a171",
		"f9b8c29b56495e19f3287ec9e47240e6",
		"0e23e4f5fb83a4802011082773a04a48",
		"f38a6b8d01b1560c5dbfdf99895e9ea5",
		"15b71c676cfb6f03d8deef760b61d778",
		"aa87f5201aa0803bdc8afb8059e90cd7"
	};
	constexpr std::array<std::string_view, 8> Q1EntryExpectedHashes{
		"fe5b14d58d75dbdc5864bef7a4aa3e51",
		"3450a4b8baa59fdcddf93d7409e038c3",
		"e9f767e93a478da574e742f20b230ab5",
		"482cad1b5d1b496e78f0e63b5596ea53",
		"be3451889f9f6f92f88e6ee7be35c71d",
		"fe5b14d58d75dbdc5864bef7a4aa3e51",
		"87a9c58fae011bab403c759ecc390e53",
		"d45a9c22647555e1c4a92802555e62cd"
	};
	constexpr std::array<std::string_view, 9> Q1FilterTrialExpectedHashes{
		"415f10788f025d5976cde60e8bf4b822",
		"c0d655932d0a6099f30f0b462dd10a7c",
		"c0d655932d0a6099f30f0b462dd10a7c",
		"cd5d1d3b12d7c2243057bc7365df1902",
		"fcda2566f657ca19c05955312767bdb9",
		"1de0bd1bbef9302d919617dacae8c54c",
		"1de0bd1bbef9302d919617dacae8c54c",
		"d0247d12addee03defca5278ed9135aa",
		"f67b54de0d48c3f4facd22dfb7ed140b"
	};
	constexpr std::array<std::string_view, 21> MediumParityExpectedHashes{
		"fe949c60fa7314c9154b390d9570b60a",
		"148649a1ec8638f27c071034a1379afd",
		"e173ba7048a2f4719cead4781af76f59",
		"2c60e91a1f0cb14ae9dfc7f0c9d07b11",
		"c24ac18afcc2b7ee184e6cc218cc176a",
		"bf00b0661b21f4b5e6a780680f66be7c",
		"4ce402ab27a7e4dd71f6de78cec8af34",
		"4ce402ab27a7e4dd71f6de78cec8af34",
		"f9b8c29b56495e19f3287ec9e47240e6",
		"2c60e91a1f0cb14ae9dfc7f0c9d07b11",
		"7bac402557d1ae9f3d3f8d9e8442c311",
		"d618fcfaac6b37468bb34cc0176a90a2",
		"b52484f05f831871b580f7eeb02354b0",
		"c0d655932d0a6099f30f0b462dd10a7c",
		"35f50fe7e42049dacdb5a569186e180e",
		"53827c1af6e9008cec0ab5109a4d10fc",
		"8c75ef49c1b11418ce007dbc809c6b78",
		"35e247243a52ae9cc2d914e4da731f0e",
		"c0d655932d0a6099f30f0b462dd10a7c",
		"cd5d1d3b12d7c2243057bc7365df1902",
		"fcda2566f657ca19c05955312767bdb9"
	};
	constexpr std::array<std::string_view, 21> HighParityExpectedHashes{
		"fe949c60fa7314c9154b390d9570b60a",
		"148649a1ec8638f27c071034a1379afd",
		"686fc52cdf3087af88d26ec4f443f2f2",
		"2197366081c9dc992388f0cc3932d539",
		"32083f72ee29d8226e129e2414dfbf9f",
		"bf00b0661b21f4b5e6a780680f66be7c",
		"8d7b188fba592f1921cda4822ee5cb0e",
		"8d7b188fba592f1921cda4822ee5cb0e",
		"f9b8c29b56495e19f3287ec9e47240e6",
		"2197366081c9dc992388f0cc3932d539",
		"eb19e052e6c2193577576a13db193d40",
		"1b14c4ab0d2332fa233f2800363460ff",
		"e24041ffaffc4b6cad8ecf6a158fd1fd",
		"1de0bd1bbef9302d919617dacae8c54c",
		"ea19ad7cb49a65dd8923a1f8c6531baf",
		"bead2b75cb99be33cde1b84c37a3ec45",
		"2282fc4ccf91b9999c8cf57110ea7c22",
		"86d7521e521143a7e7ad669020499769",
		"1de0bd1bbef9302d919617dacae8c54c",
		"d0247d12addee03defca5278ed9135aa",
		"f67b54de0d48c3f4facd22dfb7ed140b"
	};
	constexpr std::array<std::string_view, 6> FilterDiagnosticExpectedHashes{
		"e22e583bc88b9a27f80616fd94f8c35f",
		"3357331dfeb0990b1abeb54d046d7ed7",
		"f1c08750ccd65ea60d703c91e6131258",
		"ab04d1e1059d9d4e0c1abd1875e81c31",
		"3357331dfeb0990b1abeb54d046d7ed7",
		"8870cce35c892d90da61e0ee9d2eba17"
	};

	const std::filesystem::path OutputDirectory =
		Durin::Testing::CreateTestFixtureDirectory("DirectionalShadowQ0Baseline");
	std::vector<std::vector<std::byte>> Captures;
	std::vector<FCaptureStatistics> Statistics;
	Captures.reserve(Fixtures.size());
	Statistics.reserve(Fixtures.size());

	for (const FFixture& Fixture : Fixtures)
	{
		Durin::FScene Scene;
		for (size_t Index = 0; Index < Fixture.Primitives.size(); ++Index)
		{
			const FPrimitivePlacement& Placement = Fixture.Primitives[Index];
			Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(Index + 1), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Placement.bMasked ? Masked : Opaque}, 1), MakeTransform(Placement));
		}
		Durin::FDirectionalLightSceneData Directional;
		Directional.Direction = Fixture.LightDirection;
		Directional.Color = {1.0f, 1.0f, 1.0f};
		Directional.Intensity = 3.0f;
		Directional.bCastShadows = Fixture.bCastShadows;
		Scene.AddOrReplaceLight(Durin::FLightSceneId(100), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
		Durin::FlushRenderingCommands();

		auto Pixels = std::make_shared<std::vector<std::byte>>();
		Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
			[&Renderer, &Scene, &Fixture, Pixels](
				Durin::FRHICommandListImmediate& CommandList
			) {
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
									  Fixture.Name.c_str(), CaptureWidth, CaptureHeight,
									  Durin::EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource | Durin::ETextureCreateFlags::CPUReadback);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				View.ViewLocation = {Fixture.CameraTranslationX, 0.0, 0.0};
				View.ViewMatrix = Durin::Math::TranslationMatrix(
					Durin::FVector3(-Fixture.CameraTranslationX, 0.0, 0.0));
				if (Fixture.bPerspective)
				{
					const double YScale = 1.0 / std::tan(Durin::Math::DegreesToRadians(60.0) * 0.5);
					constexpr double NearClip = 1.0;
					constexpr double FarClip = 300.0;
					View.ProjectionMatrix = Durin::FMatrix(0.0);
					View.ProjectionMatrix[1][0] = YScale;
					View.ProjectionMatrix[2][1] = -YScale;
					View.ProjectionMatrix[0][2] =
						FarClip / (FarClip - NearClip);
					View.ProjectionMatrix[3][2] =
						-NearClip * FarClip / (FarClip - NearClip);
					View.ProjectionMatrix[0][3] = 1.0;
				}
				else
				{
					View.ProjectionMatrix = Durin::FMatrix(1.0);
					View.ProjectionMatrix[2][2] = -1.0;
					View.ProjectionMatrix[3][2] = 1.0;
				}
				View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
				View.ViewportWidth = CaptureWidth;
				View.ViewportHeight = CaptureHeight;
				View.Settings.Mode.RenderMode = Fixture.RenderMode;
				View.Settings.Mode.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.DirectionalShadow.DiagnosticMode =
					Fixture.DiagnosticMode;
				View.Settings.DirectionalShadow.FilterQuality =
					Fixture.FilterQuality;
				View.Settings.DirectionalShadow.Candidate = Fixture.Candidate;
				// Contact-shadow supplement stays off for the shadow-map-only baseline.
				View.Settings.DirectionalShadow.bEnableContactShadows = false;
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}), Durin::ERenderViewResult::Success);
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					CommandList, Target, 0, 0, *Pixels
				));
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		ASSERT_EQ(Pixels->size(), static_cast<size_t>(CaptureWidth) * CaptureHeight * 4u);
		if (Fixture.bCastShadows)
		{
			EXPECT_EQ(GLastCounters.ShadowSelectedLights, 1u);
			EXPECT_EQ(GLastCounters.ShadowValidReceiverViews, 1u);
			EXPECT_GT(GLastCounters.ShadowSuccessfulDraws, 0u);
			EXPECT_EQ(GLastCounters.ShadowDiagnosticViews[static_cast<size_t>(Fixture.DiagnosticMode)], 1u);
			EXPECT_EQ(GLastCounters.ShadowQualityViews[static_cast<size_t>(Fixture.FilterQuality)], 1u);
			const Durin::FDirectionalShadowFilter ExpectedFilter =
				Durin::PrepareDirectionalShadowFilter(Fixture.FilterQuality);
			EXPECT_EQ(GLastCounters.ShadowComparisonOperations, ExpectedFilter.ComparisonOperations);
			EXPECT_EQ(GLastCounters.ShadowGuardTexels, ExpectedFilter.GuardTexels);
			if (Fixture.Candidate
				== Durin::EDirectionalShadowCandidate::ThreeCascades)
			{
				EXPECT_EQ(GLastCounters.ShadowCandidate, Fixture.Candidate);
				EXPECT_EQ(GLastCounters.ShadowCascadeCount, Durin::DirectionalShadowCascadeCount);
				EXPECT_EQ(GLastCounters.ShadowComparisonOperations, 9u);
				EXPECT_EQ(
					GLastCounters.ShadowTransitionComparisonOperations, 18u
				);
				EXPECT_EQ(GLastCounters.ShadowTargetLogicalBytes, Durin::DirectionalShadowLogicalBytes);
				EXPECT_GE(GLastCounters.ShadowTargetBackendBytes, GLastCounters.ShadowTargetLogicalBytes);
				EXPECT_LE(GLastCounters.ShadowTargetBackendBytes, 64ull * 1024 * 1024);
				size_t CascadeAttempts = 0;
				for (uint32 Cascade = 0;
					 Cascade < Durin::DirectionalShadowCascadeCount; ++Cascade)
					CascadeAttempts +=
						GLastCounters.ShadowCascades[Cascade].AttemptedDraws;
				EXPECT_EQ(CascadeAttempts, GLastCounters.ShadowAttemptedDraws);
			}
		}
		else
		{
			EXPECT_EQ(GLastCounters.ShadowSelectedLights, 1u);
			EXPECT_EQ(GLastCounters.ShadowValidReceiverViews, 0u);
			EXPECT_EQ(GLastCounters.ShadowSuccessfulDraws, 0u);
			for (const size_t DiagnosticViews : GLastCounters.ShadowDiagnosticViews)
				EXPECT_EQ(DiagnosticViews, 0u);
		}
		WritePpm(OutputDirectory / (Fixture.Name + ".ppm"), *Pixels);
		Statistics.push_back(
			CalculateStatistics(Fixture.Name, *Pixels, GLastCounters)
		);
		Captures.push_back(std::move(*Pixels));
	}

	ASSERT_EQ(Captures.size(), Fixtures.size());
	ASSERT_GE(Statistics.size(), ExpectedHashes.size());
	for (size_t Index = 0; Index < ExpectedHashes.size(); ++Index)
	{
		EXPECT_EQ(Statistics[Index].Hash, ExpectedHashes[Index])
			<< Statistics[Index].Name;
	}
	for (size_t Index = 0; Index < Q1EntryExpectedHashes.size(); ++Index)
	{
		const size_t CaptureIndex = 23u + Index;
		EXPECT_EQ(Statistics[CaptureIndex].Hash, Q1EntryExpectedHashes[Index])
			<< Statistics[CaptureIndex].Name;
	}
	for (size_t Index = 0; Index < Q1FilterTrialExpectedHashes.size(); ++Index)
	{
		const size_t CaptureIndex = 31u + Index;
		EXPECT_EQ(Statistics[CaptureIndex].Hash, Q1FilterTrialExpectedHashes[Index])
			<< Statistics[CaptureIndex].Name;
	}
	for (size_t Index = 0; Index < MediumParityExpectedHashes.size(); ++Index)
	{
		EXPECT_EQ(Statistics[MediumParityStart + Index].Hash, MediumParityExpectedHashes[Index])
			<< Statistics[MediumParityStart + Index].Name;
		EXPECT_EQ(Statistics[HighParityStart + Index].Hash, HighParityExpectedHashes[Index])
			<< Statistics[HighParityStart + Index].Name;
	}
	for (size_t Index = 0; Index < FilterDiagnosticExpectedHashes.size(); ++Index)
	{
		EXPECT_EQ(Statistics[FilterDiagnosticStart + Index].Hash, FilterDiagnosticExpectedHashes[Index])
			<< Statistics[FilterDiagnosticStart + Index].Name;
	}
	EXPECT_NE(Captures[CascadeFixtureStart], Captures[8]);
	const std::array<size_t, 3> MotionChangedPixels{
		CountChangedPixels(Captures[9], Captures[10], 2),
		CountChangedPixels(Captures[10], Captures[11], 2),
		CountChangedPixels(Captures[9], Captures[12], 2)
	};
	const std::array<size_t, 2> Q1EntryMotionChangedPixels{
		CountChangedPixels(Captures[28], Captures[29], 2),
		CountChangedPixels(Captures[28], Captures[30], 2)
	};
	EXPECT_LT(MotionChangedPixels[0], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MotionChangedPixels[1], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MotionChangedPixels[2], CaptureWidth * CaptureHeight / 4u);
	EXPECT_LE(Q1EntryMotionChangedPixels[0], 256u);
	EXPECT_LE(Q1EntryMotionChangedPixels[1], 256u);
	EXPECT_NE(Captures[3], Captures[4]);
	EXPECT_EQ(Captures[6], Captures[7]);
	EXPECT_NE(Captures[2], Captures[8]);
	EXPECT_NE(Captures[3], Captures[8]);
	for (size_t Index = 13; Index < LowFixtureCount; ++Index)
	{
		EXPECT_NE(Captures[Index], Captures[3]) << Fixtures[Index].Name;
	}
	EXPECT_NE(Captures[14], Captures[15]);
	EXPECT_NE(Captures[15], Captures[16]);
	EXPECT_EQ(Captures[8], Captures[20]);
	EXPECT_EQ(Captures[21], Captures[22]);
	const std::array<double, 3> ShadowOnlyHighFrequencyFraction{
		CalculateShadowOnlyHighFrequencyFraction(Captures[23], Captures[31]),
		CalculateShadowOnlyHighFrequencyFraction(Captures[32], Captures[31]),
		CalculateShadowOnlyHighFrequencyFraction(Captures[36], Captures[31])
	};
	const std::array<size_t, 2> MediumMotion{
		CountChangedPixels(Captures[33], Captures[34], 2),
		CountChangedPixels(Captures[33], Captures[35], 2)
	};
	const std::array<size_t, 2> HighMotion{
		CountChangedPixels(Captures[37], Captures[38], 2),
		CountChangedPixels(Captures[37], Captures[39], 2)
	};
	const std::array<size_t, 3> MediumQ0Motion{
		CountChangedPixels(Captures[MediumParityStart + 9u], Captures[MediumParityStart + 10u], 2),
		CountChangedPixels(Captures[MediumParityStart + 10u], Captures[MediumParityStart + 11u], 2),
		CountChangedPixels(Captures[MediumParityStart + 9u], Captures[MediumParityStart + 12u], 2)
	};
	const std::array<size_t, 3> HighQ0Motion{
		CountChangedPixels(Captures[HighParityStart + 9u], Captures[HighParityStart + 10u], 2),
		CountChangedPixels(Captures[HighParityStart + 10u], Captures[HighParityStart + 11u], 2),
		CountChangedPixels(Captures[HighParityStart + 9u], Captures[HighParityStart + 12u], 2)
	};
	for (const size_t Changed : MediumMotion)
		EXPECT_LE(Changed, 224u);
	EXPECT_LT(MediumQ0Motion[0], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MediumQ0Motion[1], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MediumQ0Motion[2], CaptureWidth * CaptureHeight / 4u);
	EXPECT_EQ(HighMotion[0], 32u);
	EXPECT_EQ(HighMotion[1], 190u);
	EXPECT_LE(ShadowOnlyHighFrequencyFraction[1], ShadowOnlyHighFrequencyFraction[0] * 0.95);
	EXPECT_LE(ShadowOnlyHighFrequencyFraction[2], ShadowOnlyHighFrequencyFraction[1] * 0.96);
	const size_t LowTransitionWidth = MaximumTransitionWidth(Captures[23]);
	const size_t MediumTransitionWidth =
		MaximumTransitionWidth(Captures[MediumParityStart + 13u]);
	const size_t HighTransitionWidth =
		MaximumTransitionWidth(Captures[HighParityStart + 13u]);
	EXPECT_LE(MediumTransitionWidth, LowTransitionWidth + 4u);
	EXPECT_LE(HighTransitionWidth, LowTransitionWidth + 8u);
	EXPECT_EQ(Captures[MediumParityStart], Captures[0]);
	EXPECT_EQ(Captures[MediumParityStart + 6u], Captures[MediumParityStart + 7u]);
	EXPECT_EQ(Captures[HighParityStart + 6u], Captures[HighParityStart + 7u]);
	EXPECT_EQ(Captures[MediumParityStart + 8u], Captures[8]);
	EXPECT_EQ(Captures[HighParityStart + 8u], Captures[8]);
	EXPECT_NE(Captures[MediumParityStart + 3u], Captures[MediumParityStart + 4u]);
	EXPECT_NE(Captures[HighParityStart + 3u], Captures[HighParityStart + 4u]);
	std::cout << std::fixed << std::setprecision(6)
			  << "Directional shadow-only high-frequency fraction Low/Medium/High: "
			  << ShadowOnlyHighFrequencyFraction[0] << ", "
			  << ShadowOnlyHighFrequencyFraction[1] << ", "
			  << ShadowOnlyHighFrequencyFraction[2] << '\n'
			  << "Directional shadow trial motion Medium: " << MediumMotion[0]
			  << ", " << MediumMotion[1] << "; High: " << HighMotion[0]
			  << ", " << HighMotion[1] << '\n'
			  << "Directional shadow Q0 motion Medium: " << MediumQ0Motion[0]
			  << ", " << MediumQ0Motion[1] << ", " << MediumQ0Motion[2]
			  << "; High: " << HighQ0Motion[0] << ", " << HighQ0Motion[1]
			  << ", " << HighQ0Motion[2] << '\n'
			  << "Directional shadow transition width Low/Medium/High: "
			  << LowTransitionWidth << ", " << MediumTransitionWidth << ", "
			  << HighTransitionWidth << '\n';
	WriteMetrics(OutputDirectory / "baseline-metrics.json", Statistics, MotionChangedPixels, Q1EntryMotionChangedPixels, ShadowOnlyHighFrequencyFraction, MediumMotion, HighMotion);
	std::cout << "Directional shadow Q0 baseline artifacts: "
			  << OutputDirectory.string() << '\n'
			  << "Directional shadow Q1 Low entry motion pixels: "
			  << Q1EntryMotionChangedPixels[0] << ", "
			  << Q1EntryMotionChangedPixels[1] << '\n';

	Durin::SetViewRenderCounterSink(nullptr);
	RendererLifecycle.Shutdown();
	Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
		[&](Durin::FRHICommandListImmediate&) { Quad->ReleaseResources(); }
	);
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}

TEST(FDirectionalShadowBaselineVulkanTests, ContactShadowRunsAndDarkensNearFieldBounded)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::FRHIInitializationContext::Headless());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	Durin::FRendererModule Renderer;
	Durin::FModuleTestHarness RendererLifecycle(
		"DirectionalContactShadowRendererTest"
	);
	RendererLifecycle.Start(Renderer);
	Durin::SetViewRenderCounterSink(CaptureCounters);

	auto Quad = MakeQuadRenderData();
	Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(Quad->InitResources(CommandList));
		}
	);
	Durin::FlushRenderingCommands();
	auto Opaque = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque,
		{0.72, 0.72, 0.72},
		Durin::EMaterialShadingModel::Lit,
		{0.0, 0.0, 1.1}
	);

	// Ground quad plus a floating occluder: the exact contact-detachment
	// scenario where necessary bias leaves a detached shadow the screen-space
	// supplement should refill.
	Durin::FScene Scene;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(1), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1), MakeTransform({.Translation = {0.0, 0.0, -0.5}, .Scale = {0.82, 0.82, 1.0}}));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1), MakeTransform({.Translation = {-0.18, 0.08, -0.4}, .Scale = {0.22, 0.18, 1.0}}));
	// A vertical wall whose visible face is back-facing relative to the light:
	// the screen-space supplement must NOT self-occlude it. The camera looks
	// toward -z (projection maps smaller z to nearer), so the floor at negative
	// z presents its +z face, which faces the light.
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(3), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1), MakeTransform({.Translation = {-0.35, 0.0, -0.45}, .Scale = {0.25, 0.25, 1.0}, .RotationYDegrees = 90.0}));
	Durin::FDirectionalLightSceneData Directional;
	Directional.Direction = {0.35, 0.2, -1.0};
	Directional.Color = {1.0f, 1.0f, 1.0f};
	Directional.Intensity = 3.0f;
	Directional.bCastShadows = true;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(100), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Durin::FlushRenderingCommands();

	auto RenderCapture = [&](bool bEnableContactShadows,
							 bool bShowContactShadowDebug,
							 std::vector<std::byte>& OutPixels,
							 bool bPerspective = false,
							 std::vector<std::byte>* HDRSceneColorPixels = nullptr,
							 std::vector<std::byte>* HDRPostProcessInputPixels = nullptr,
							 bool bEnableGBufferQualification = false,
							 std::vector<std::byte>* GBufferMaterialPixels = nullptr,
							 std::vector<std::byte>* GBufferSurfacePixels = nullptr,
							 Durin::EGBufferDebugMode GBufferDebugMode =
								 Durin::EGBufferDebugMode::Disabled,
							 std::vector<std::byte>* GBufferNormalsPixels = nullptr,
							 std::vector<std::byte>* GBufferEmissivePixels = nullptr,
							 Durin::ERenderMode RenderMode = Durin::ERenderMode::Lit,
							 bool bEnableDeferredDirectional = false,
							 Durin::EDeferredDirectionalDebugMode DeferredDebugMode =
								 Durin::EDeferredDirectionalDebugMode::Disabled,
							 std::vector<std::byte>* DeferredDirectionalPixels = nullptr,
							 Durin::EDirectionalShadowCandidate ShadowCandidate =
								 Durin::EDirectionalShadowCandidate::SingleMap,
							 Durin::EDirectionalShadowFilterQuality ShadowFilter =
							 Durin::EDirectionalShadowFilterQuality::Low,
							 Durin::EDirectionalShadowDiagnosticMode ShadowDiagnostic =
								 Durin::EDirectionalShadowDiagnosticMode::Lit,
								 float AspectRatioConstraint = 0.0f,
								 bool bForceFragmentContactVisibility = false)
		-> Durin::FViewRenderCounters {
		auto Pixels = std::make_shared<std::vector<std::byte>>();
		GHDRSceneColorPixels = HDRSceneColorPixels;
		GHDRPostProcessInputPixels = HDRPostProcessInputPixels;
		Durin::SetHDRSceneColorCaptureSink(CaptureHDRSceneColor);
		GGBufferMaterialPixels = GBufferMaterialPixels;
		GGBufferNormalsPixels = GBufferNormalsPixels;
		GGBufferSurfacePixels = GBufferSurfacePixels;
		GGBufferEmissivePixels = GBufferEmissivePixels;
		Durin::SetGBufferCaptureSink(CaptureGBuffer);
		GDeferredDirectionalPixels = DeferredDirectionalPixels;
		Durin::SetDeferredDirectionalCaptureSink(
			CaptureDeferredDirectional
		);
		Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
			[&Renderer, &Scene, bEnableContactShadows,
			 bShowContactShadowDebug, bPerspective,
			 bEnableGBufferQualification, GBufferDebugMode, RenderMode,
			 bEnableDeferredDirectional, DeferredDebugMode, ShadowCandidate,
			 ShadowFilter, ShadowDiagnostic, AspectRatioConstraint,
			 bForceFragmentContactVisibility, Pixels](
				Durin::FRHICommandListImmediate& CommandList
			) {
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
									  "ContactShadowCapture", CaptureWidth, CaptureHeight,
									  Durin::EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource | Durin::ETextureCreateFlags::CPUReadback);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				if (bPerspective)
				{
					// Look down world -Z while preserving world X/Y as screen X/Y.
					View.ViewLocation = {0.0, 0.0, 1.0};
					View.ViewMatrix = Durin::FMatrix(0.0);
					View.ViewMatrix[2][0] = -1.0;
					View.ViewMatrix[3][0] = 1.0;
					View.ViewMatrix[0][1] = 1.0;
					View.ViewMatrix[1][2] = -1.0;
					View.ViewMatrix[3][3] = 1.0;
					constexpr double NearClip = 0.1;
					constexpr double FarClip = 10.0;
					const double YScale = 1.0 / std::tan(Durin::Math::DegreesToRadians(60.0) * 0.5);
					View.ProjectionMatrix = Durin::FMatrix(0.0);
					View.ProjectionMatrix[1][0] = YScale;
					View.ProjectionMatrix[2][1] = -YScale;
					View.ProjectionMatrix[0][2] =
						FarClip / (FarClip - NearClip);
					View.ProjectionMatrix[3][2] =
						-NearClip * FarClip / (FarClip - NearClip);
					View.ProjectionMatrix[0][3] = 1.0;
				}
				else
				{
					View.ViewMatrix = Durin::FMatrix(1.0);
					View.ProjectionMatrix = Durin::FMatrix(1.0);
					View.ProjectionMatrix[2][2] = -1.0;
					View.ProjectionMatrix[3][2] = 0.0;
				}
				View.ViewProjectionMatrix =
					View.ProjectionMatrix * View.ViewMatrix;
				View.ViewportWidth = CaptureWidth;
				View.ViewportHeight = CaptureHeight;
				View.AspectRatioConstraint = AspectRatioConstraint;
				View.Settings.Mode.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.Mode.RenderMode = RenderMode;
				View.Settings.DirectionalShadow.Candidate = ShadowCandidate;
				View.Settings.DirectionalShadow.FilterQuality = ShadowFilter;
				View.Settings.DirectionalShadow.DiagnosticMode = ShadowDiagnostic;
				View.Settings.DirectionalShadow.bEnableContactShadows = bEnableContactShadows;
				View.Settings.DirectionalShadow.bShowContactDebug =
					bShowContactShadowDebug;
				View.Settings.DirectionalShadow.ContactRoutePreference =
					bForceFragmentContactVisibility
						? Durin::EContactShadowRoutePreference::Fragment
						: Durin::EContactShadowRoutePreference::Auto;
				Durin::FSceneViewRenderOptions RenderOptions;
				RenderOptions.bEnableGBufferQualification =
					bEnableGBufferQualification;
				RenderOptions.GBufferDebugMode = GBufferDebugMode;
				RenderOptions.bEnableDeferredDirectionalQualification =
					bEnableDeferredDirectional;
				RenderOptions.DeferredDirectionalDebugMode = DeferredDebugMode;
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, RenderOptions), Durin::ERenderViewResult::Success);
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					CommandList, Target, 0, 0, *Pixels
				));
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		Durin::SetHDRSceneColorCaptureSink(nullptr);
		Durin::SetGBufferCaptureSink(nullptr);
		Durin::SetDeferredDirectionalCaptureSink(nullptr);
		GHDRSceneColorPixels = nullptr;
		GHDRPostProcessInputPixels = nullptr;
		GGBufferMaterialPixels = nullptr;
		GGBufferNormalsPixels = nullptr;
		GGBufferSurfacePixels = nullptr;
		GGBufferEmissivePixels = nullptr;
		GDeferredDirectionalPixels = nullptr;
		EXPECT_EQ(Pixels->size(), static_cast<size_t>(CaptureWidth) * CaptureHeight * 4u);
		OutPixels = std::move(*Pixels);
		return GLastCounters;
	};

	std::vector<std::byte> PixelsOff;
	std::vector<std::byte> HDRSceneOff;
	std::vector<std::byte> HDRInputOff;
	const Durin::FViewRenderCounters CountersOff =
		RenderCapture(false, false, PixelsOff, false, &HDRSceneOff, &HDRInputOff);
	std::vector<std::byte> PixelsOn;
	std::vector<std::byte> HDRSceneOn;
	std::vector<std::byte> HDRInputOn;
	const Durin::FViewRenderCounters CountersOn =
		RenderCapture(true, false, PixelsOn, false, &HDRSceneOn, &HDRInputOn);
	std::vector<std::byte> GBufferPixels;
	std::vector<std::byte> GBufferMaterialPixels;
	std::vector<std::byte> GBufferNormalsPixels;
	std::vector<std::byte> GBufferSurfacePixels;
	std::vector<std::byte> GBufferEmissivePixels;
	const Durin::FViewRenderCounters GBufferCounters =
		RenderCapture(false, false, GBufferPixels, false, nullptr, nullptr, true, &GBufferMaterialPixels, &GBufferSurfacePixels, Durin::EGBufferDebugMode::Disabled, &GBufferNormalsPixels, &GBufferEmissivePixels);

	// Stage 2 keeps the deferred target isolated and explicitly unshadowed.
	// Disable the forward shadow for this A/B slice, then restore it before the
	// contact/shadow assertions below.
	Directional.bCastShadows = false;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(100), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Durin::FlushRenderingCommands();
	std::vector<std::byte> ForwardOnlyOutput;
	std::vector<std::byte> ForwardOnlyHDR;
	RenderCapture(false, false, ForwardOnlyOutput, false, &ForwardOnlyHDR);
	std::vector<std::byte> DeferredOutput;
	std::vector<std::byte> DeferredForwardHDR;
	std::vector<std::byte> DeferredHDR;
	const Durin::FViewRenderCounters DeferredCounters = RenderCapture(
		false, false, DeferredOutput, false, &DeferredForwardHDR,
		nullptr, false, nullptr, nullptr,
		Durin::EGBufferDebugMode::Disabled, nullptr, nullptr,
		Durin::ERenderMode::Lit, true,
		Durin::EDeferredDirectionalDebugMode::Final, &DeferredHDR
	);
	EXPECT_EQ(DeferredOutput, ForwardOnlyOutput);
	EXPECT_EQ(DeferredForwardHDR, ForwardOnlyHDR);
	EXPECT_EQ(DeferredCounters.DeferredDirectionalEnabledViews, 1u);
	EXPECT_EQ(DeferredCounters.DeferredDirectionalUnavailableViews, 0u);
	EXPECT_EQ(DeferredCounters.DeferredDirectionalPassFailures, 0u);
	EXPECT_EQ(DeferredCounters.DeferredDirectionalDebugViews, 1u);
	EXPECT_EQ(DeferredCounters.DeferredDirectionalOutputBytes, Durin::FDeferredDirectionalLightingRenderer::CalculateTargetBytes(CaptureWidth, CaptureHeight));
	ASSERT_EQ(DeferredHDR.size(), ForwardOnlyHDR.size());
	ASSERT_EQ(DeferredHDR.size(), static_cast<size_t>(CaptureWidth) * CaptureHeight * 8u);

	auto DecodeHalf = [](const uint8* Bytes) {
		const uint16 Bits = static_cast<uint16>(Bytes[0])
								   | static_cast<uint16>(Bytes[1] << 8);
		const bool bNegative = (Bits & 0x8000u) != 0;
		const uint32 Exponent = (Bits >> 10) & 0x1fu;
		const uint32 Mantissa = Bits & 0x3ffu;
		double Value = 0.0;
		if (Exponent == 0)
			Value = std::ldexp(static_cast<double>(Mantissa), -24);
		else if (Exponent == 31)
			Value = Mantissa == 0 ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
		else
			Value = std::ldexp(
				static_cast<double>(1024u + Mantissa),
				static_cast<int>(Exponent) - 25
			);
		return bNegative ? -Value : Value;
	};
	auto ToDisplayByte = [](double SceneLinear) {
		const double X = std::max(SceneLinear, 0.0);
		const double Mapped = std::clamp(
			(X * (2.51 * X + 0.03))
				/ (X * (2.43 * X + 0.59) + 0.14),
			0.0, 1.0
		);
		const double Encoded = Mapped <= 0.0031308 ? 12.92 * Mapped : 1.055 * std::pow(Mapped, 1.0 / 2.4) - 0.055;
		return static_cast<int>(std::lround(
			std::clamp(Encoded, 0.0, 1.0) * 255.0
		));
	};
	auto ExpectDeferredParity = [&](const std::vector<std::byte>& Forward,
									const std::vector<std::byte>& Deferred,
									const std::vector<std::byte>& Surface,
									std::string_view Label = {}) {
		SCOPED_TRACE(Label);
		ASSERT_EQ(Forward.size(), Deferred.size());
		ASSERT_EQ(Forward.size(), static_cast<size_t>(CaptureWidth) * CaptureHeight * 8u);
		std::vector<int> DisplayErrors;
		double DisplayErrorSum = 0.0;
		for (size_t Pixel = 0;
			 Pixel < Surface.size() / 4u; ++Pixel)
		{
			const bool bValid =
				Surface[Pixel * 4u + 3u] != 0u;
			const size_t HDROffset = Pixel * 8u;
			if (!bValid)
			{
				EXPECT_EQ(std::memcmp(Deferred.data() + HDROffset, Forward.data() + HDROffset, 8), 0);
				continue;
			}
			for (size_t Channel = 0; Channel < 3; ++Channel)
			{
				const double ForwardValue = DecodeHalf(
					Forward.data() + HDROffset + Channel * 2u
				);
				const double DeferredValue = DecodeHalf(
					Deferred.data() + HDROffset + Channel * 2u
				);
				ASSERT_TRUE(std::isfinite(ForwardValue));
				ASSERT_TRUE(std::isfinite(DeferredValue));
				const int Error = std::abs(
					ToDisplayByte(ForwardValue) - ToDisplayByte(DeferredValue)
				);
				DisplayErrors.push_back(Error);
				DisplayErrorSum += Error;
			}
			EXPECT_LE(std::abs(DecodeHalf(Forward.data() + HDROffset + 6u) - DecodeHalf(Deferred.data() + HDROffset + 6u)), 1.0 / 510.0);
		}
		ASSERT_FALSE(DisplayErrors.empty());
		std::ranges::sort(DisplayErrors);
		EXPECT_LE(DisplayErrorSum / DisplayErrors.size(), 1.0);
		EXPECT_LE(DisplayErrors[static_cast<size_t>(0.99 * static_cast<double>(DisplayErrors.size() - 1u))], 3);
		EXPECT_LE(DisplayErrors.back(), 18);
	};
	ExpectDeferredParity(ForwardOnlyHDR, DeferredHDR, GBufferSurfacePixels);
	std::vector<std::byte> HybridOutput;
	std::vector<std::byte> HybridHDR;
	const Durin::FViewRenderCounters HybridCounters = RenderCapture(
		false, false, HybridOutput, false, &HybridHDR, nullptr,
		false, nullptr, nullptr, Durin::EGBufferDebugMode::Disabled,
		nullptr, nullptr, Durin::ERenderMode::Lit, false,
		Durin::EDeferredDirectionalDebugMode::Disabled, nullptr,
		Durin::EDirectionalShadowCandidate::SingleMap,
		Durin::EDirectionalShadowFilterQuality::Low,
		Durin::EDirectionalShadowDiagnosticMode::Lit, 0.0f
	);
	ExpectDeferredParity(ForwardOnlyHDR, HybridHDR, GBufferSurfacePixels);
	EXPECT_EQ(HybridCounters.HybridDeferredEnabledViews, 1u);
	EXPECT_EQ(HybridCounters.HybridDeferredUnavailableViews, 0u);

	auto Translucent = MakeMaterial(Durin::EMaterialBlendMode::Translucent, {0.1, 0.8, 0.25}, Durin::EMaterialShadingModel::Lit, Durin::FVector3(0.0), 0.45);
	auto MixedUnlit = MakeMaterial(Durin::EMaterialBlendMode::Opaque, {0.12, 0.18, 0.75}, Durin::EMaterialShadingModel::Unlit, {2.0, 0.25, 0.1});
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Translucent}, 1), MakeTransform({.Translation = {-0.18, 0.08, -0.4}, .Scale = {0.22, 0.18, 1.0}}));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(3), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{MixedUnlit}, 1), MakeTransform({.Translation = {-0.35, 0.0, -0.45}, .Scale = {0.25, 0.25, 1.0}, .RotationYDegrees = 90.0}));
	Durin::FlushRenderingCommands();
	std::vector<std::byte> MixedForwardOutput;
	std::vector<std::byte> MixedForwardHDR;
	RenderCapture(false, false, MixedForwardOutput, false, &MixedForwardHDR);
	std::vector<std::byte> MixedHybridOutput;
	std::vector<std::byte> MixedHybridHDR;
	std::vector<std::byte> MixedSurface;
	const Durin::FViewRenderCounters MixedHybridCounters = RenderCapture(
		false, false, MixedHybridOutput, false, &MixedHybridHDR, nullptr,
		false, nullptr, &MixedSurface, Durin::EGBufferDebugMode::Disabled,
		nullptr, nullptr, Durin::ERenderMode::Lit, false,
		Durin::EDeferredDirectionalDebugMode::Disabled, nullptr,
		Durin::EDirectionalShadowCandidate::SingleMap,
		Durin::EDirectionalShadowFilterQuality::Low,
		Durin::EDirectionalShadowDiagnosticMode::Lit, 0.0f
	);
	ExpectDeferredParity(MixedForwardHDR, MixedHybridHDR, MixedSurface, "retained-unlit-translucent");
	EXPECT_EQ(MixedHybridCounters.GBufferAttemptedDraws, 1u);
	EXPECT_EQ(MixedHybridCounters.GBufferSkippedDraws, 2u);
	EXPECT_EQ(MixedHybridCounters.HybridDeferredEnabledViews, 1u);
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1), MakeTransform({.Translation = {-0.18, 0.08, -0.4}, .Scale = {0.22, 0.18, 1.0}}));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(3), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1), MakeTransform({.Translation = {-0.35, 0.0, -0.45}, .Scale = {0.25, 0.25, 1.0}, .RotationYDegrees = 90.0}));
	Durin::FlushRenderingCommands();

	const std::array DeferredDebugModes{
		Durin::EDeferredDirectionalDebugMode::DecodedMaterial,
		Durin::EDeferredDirectionalDebugMode::Directional,
		Durin::EDeferredDirectionalDebugMode::Local,
		Durin::EDeferredDirectionalDebugMode::Environment,
		Durin::EDeferredDirectionalDebugMode::Emissive,
		Durin::EDeferredDirectionalDebugMode::Alpha,
		Durin::EDeferredDirectionalDebugMode::Final
	};
	std::vector<std::vector<std::byte>> DeferredDebugImages;
	for (const Durin::EDeferredDirectionalDebugMode Mode : DeferredDebugModes)
	{
		auto& Image = DeferredDebugImages.emplace_back();
		std::vector<std::byte> IgnoredOutput;
		const Durin::FViewRenderCounters Counters = RenderCapture(
			false, false, IgnoredOutput, false, nullptr, nullptr,
			false, nullptr, nullptr, Durin::EGBufferDebugMode::Disabled,
			nullptr, nullptr, Durin::ERenderMode::Lit, true, Mode, &Image
		);
		EXPECT_EQ(Counters.DeferredDirectionalEnabledViews, 1u);
		EXPECT_EQ(Counters.DeferredDirectionalDebugViews, 1u);
		EXPECT_EQ(Counters.DeferredDirectionalPassFailures, 0u);
		EXPECT_EQ(Image.size(), DeferredHDR.size());
		EXPECT_TRUE(std::ranges::any_of(Image, [](uint8 Value) { return Value != 0u; }));
	}
	EXPECT_NE(DeferredDebugImages.front(), DeferredDebugImages.back());
	std::vector<std::byte> DeferredPerspectiveOutput;
	std::vector<std::byte> DeferredPerspectiveForwardHDR;
	std::vector<std::byte> DeferredPerspectiveHDR;
	std::vector<std::byte> DeferredPerspectiveSurface;
	const Durin::FViewRenderCounters DeferredPerspectiveCounters =
		RenderCapture(false, false, DeferredPerspectiveOutput, true, &DeferredPerspectiveForwardHDR, nullptr, false, nullptr, &DeferredPerspectiveSurface, Durin::EGBufferDebugMode::Disabled, nullptr, nullptr, Durin::ERenderMode::Lit, true, Durin::EDeferredDirectionalDebugMode::Final, &DeferredPerspectiveHDR);
	EXPECT_EQ(DeferredPerspectiveCounters.DeferredDirectionalEnabledViews, 1u);
	EXPECT_EQ(DeferredPerspectiveCounters.DeferredDirectionalPassFailures, 0u);
	EXPECT_EQ(DeferredPerspectiveHDR.size(), DeferredHDR.size());
	ExpectDeferredParity(DeferredPerspectiveForwardHDR, DeferredPerspectiveHDR, DeferredPerspectiveSurface);

	std::vector<std::byte> ConstrainedOutput;
	std::vector<std::byte> ConstrainedForwardHDR;
	std::vector<std::byte> ConstrainedDeferredHDR;
	std::vector<std::byte> ConstrainedSurface;
	const Durin::FViewRenderCounters ConstrainedCounters = RenderCapture(
		false, false, ConstrainedOutput, false, &ConstrainedForwardHDR,
		nullptr, false, nullptr, &ConstrainedSurface,
		Durin::EGBufferDebugMode::Disabled, nullptr, nullptr,
		Durin::ERenderMode::Lit, true,
		Durin::EDeferredDirectionalDebugMode::Final,
		&ConstrainedDeferredHDR,
		Durin::EDirectionalShadowCandidate::SingleMap,
		Durin::EDirectionalShadowFilterQuality::Low,
		Durin::EDirectionalShadowDiagnosticMode::Lit, 16.0f / 9.0f
	);
	EXPECT_EQ(ConstrainedCounters.DeferredDirectionalEnabledViews, 1u);
	EXPECT_EQ(ConstrainedCounters.DeferredDirectionalPassFailures, 0u);
	ExpectDeferredParity(
		ConstrainedForwardHDR, ConstrainedDeferredHDR, ConstrainedSurface,
		"constrained-aspect"
	);

	Directional.bCastShadows = true;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(100), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Durin::FlushRenderingCommands();

	// Stage 3 consumes the same selected shadow map, comparison sampler, and
	// prepared receiver uniform as forward. Exercise every filter tier against
	// the current GBuffer while keeping the isolated target non-authoritative.
	const std::array ShadowFilterTiers{
		Durin::EDirectionalShadowFilterQuality::Low,
		Durin::EDirectionalShadowFilterQuality::Medium,
		Durin::EDirectionalShadowFilterQuality::High
	};
	std::vector<std::byte> LowShadowDeferredHDR;
	for (const Durin::EDirectionalShadowFilterQuality Quality :
		 ShadowFilterTiers)
	{
		std::vector<std::byte> ShadowOutput;
		std::vector<std::byte> ShadowForwardHDR;
		std::vector<std::byte> ShadowDeferredHDR;
		const Durin::FViewRenderCounters ShadowDeferredCounters =
			RenderCapture(false, false, ShadowOutput, false, &ShadowForwardHDR, nullptr, false, nullptr, nullptr, Durin::EGBufferDebugMode::Disabled, nullptr, nullptr, Durin::ERenderMode::Lit, true, Durin::EDeferredDirectionalDebugMode::Final, &ShadowDeferredHDR, Durin::EDirectionalShadowCandidate::SingleMap, Quality);
		EXPECT_EQ(ShadowDeferredCounters.DeferredDirectionalEnabledViews, 1u);
		EXPECT_EQ(ShadowDeferredCounters.DeferredDirectionalPassFailures, 0u);
		ExpectDeferredParity(
			ShadowForwardHDR, ShadowDeferredHDR, GBufferSurfacePixels
		);
		if (Quality == Durin::EDirectionalShadowFilterQuality::Low)
		{
			EXPECT_EQ(ShadowOutput, PixelsOff);
			LowShadowDeferredHDR = ShadowDeferredHDR;
		}
	}
	EXPECT_NE(LowShadowDeferredHDR, DeferredHDR);

	std::vector<std::byte> CascadeOutput;
	std::vector<std::byte> CascadeForwardHDR;
	std::vector<std::byte> CascadeDeferredHDR;
	std::vector<std::byte> CascadeSurface;
	const Durin::FViewRenderCounters CascadeCounters = RenderCapture(
		false, false, CascadeOutput, true, &CascadeForwardHDR, nullptr,
		false, nullptr, &CascadeSurface, Durin::EGBufferDebugMode::Disabled,
		nullptr, nullptr, Durin::ERenderMode::Lit, true,
		Durin::EDeferredDirectionalDebugMode::Final, &CascadeDeferredHDR,
		Durin::EDirectionalShadowCandidate::ThreeCascades,
		Durin::EDirectionalShadowFilterQuality::Medium,
		Durin::EDirectionalShadowDiagnosticMode::CascadeIndex
	);
	EXPECT_EQ(CascadeCounters.DeferredDirectionalEnabledViews, 1u);
	EXPECT_EQ(CascadeCounters.DeferredDirectionalPassFailures, 0u);
	EXPECT_EQ(CascadeCounters.ShadowCascadeCount, Durin::DirectionalShadowCascadeCount);
	ExpectDeferredParity(
		CascadeForwardHDR, CascadeDeferredHDR, CascadeSurface
	);

	auto SetFixtureMaterial = [&](const Durin::FMaterialRenderProxyRef& Material) {
		Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(1), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1), MakeTransform({.Translation = {0.0, 0.0, -0.5}, .Scale = {0.82, 0.82, 1.0}}));
		Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1), MakeTransform({.Translation = {-0.18, 0.08, -0.4}, .Scale = {0.22, 0.18, 1.0}}));
		Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(3), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1), MakeTransform({.Translation = {-0.35, 0.0, -0.45}, .Scale = {0.25, 0.25, 1.0}, .RotationYDegrees = 90.0}));
		Durin::FlushRenderingCommands();
	};
	auto CaptureDeferredTerm = [&](std::vector<std::byte>& Forward,
								   std::vector<std::byte>& Deferred,
								   std::vector<std::byte>& Surface) {
		std::vector<std::byte> Output;
		const Durin::FViewRenderCounters Counters = RenderCapture(
			false, false, Output, false, &Forward, nullptr, false,
			nullptr, &Surface, Durin::EGBufferDebugMode::Disabled,
			nullptr, nullptr, Durin::ERenderMode::Lit, true,
			Durin::EDeferredDirectionalDebugMode::Final, &Deferred
		);
		EXPECT_EQ(Counters.DeferredDirectionalEnabledViews, 1u);
		EXPECT_EQ(Counters.DeferredDirectionalPassFailures, 0u);
		ExpectDeferredParity(Forward, Deferred, Surface);
	};

	Scene.RemoveLight(Durin::FLightSceneId(100));
	Durin::FlushRenderingCommands();
	auto PureLit = MakeMaterial(Durin::EMaterialBlendMode::Opaque, {0.72, 0.72, 0.72}, Durin::EMaterialShadingModel::Lit);
	SetFixtureMaterial(PureLit);
	std::vector<std::byte> NoLightForward;
	std::vector<std::byte> NoLightDeferred;
	std::vector<std::byte> NoLightSurface;
	CaptureDeferredTerm(NoLightForward, NoLightDeferred, NoLightSurface);

	auto EmissiveOnly = MakeMaterial(Durin::EMaterialBlendMode::Opaque, {0.0, 0.0, 0.0}, Durin::EMaterialShadingModel::Lit, {4.0, 2.0, 0.5});
	SetFixtureMaterial(EmissiveOnly);
	std::vector<std::byte> EmissiveOnlyForward;
	std::vector<std::byte> EmissiveOnlyDeferred;
	std::vector<std::byte> EmissiveOnlySurface;
	CaptureDeferredTerm(
		EmissiveOnlyForward, EmissiveOnlyDeferred, EmissiveOnlySurface
	);
	bool bDeferredEmissiveAboveOne = false;
	for (size_t Offset = 0; Offset + 5 < EmissiveOnlyDeferred.size();
		 Offset += 8)
	{
		bDeferredEmissiveAboveOne = bDeferredEmissiveAboveOne
									|| DecodeHalf(EmissiveOnlyDeferred.data() + Offset) > 1.0
									|| DecodeHalf(EmissiveOnlyDeferred.data() + Offset + 2) > 1.0
									|| DecodeHalf(EmissiveOnlyDeferred.data() + Offset + 4) > 1.0;
	}
	EXPECT_TRUE(bDeferredEmissiveAboveOne);
	EXPECT_NE(EmissiveOnlyDeferred, NoLightDeferred);

	Directional.bCastShadows = false;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(100), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	SetFixtureMaterial(PureLit);
	std::vector<std::byte> DirectionalOnlyForward;
	std::vector<std::byte> DirectionalOnlyDeferred;
	std::vector<std::byte> DirectionalOnlySurface;
	CaptureDeferredTerm(DirectionalOnlyForward, DirectionalOnlyDeferred, DirectionalOnlySurface);
	EXPECT_NE(DirectionalOnlyDeferred, NoLightDeferred);

	Scene.RemoveLight(Durin::FLightSceneId(100));
	Durin::FPointLightSceneData Point;
	Point.Position = {-0.35, 0.15, 1.0};
	Point.Color = {1.0f, 0.15f, 0.05f};
	Point.Intensity = 5.0f;
	Point.Range = 6.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(200), std::make_unique<Durin::FPointLightSceneProxy>(Point));
	Durin::FlushRenderingCommands();
	std::vector<std::byte> PointOnlyForward;
	std::vector<std::byte> PointOnlyDeferred;
	std::vector<std::byte> PointOnlySurface;
	CaptureDeferredTerm(PointOnlyForward, PointOnlyDeferred, PointOnlySurface);
	EXPECT_NE(PointOnlyDeferred, NoLightDeferred);

	Scene.RemoveLight(Durin::FLightSceneId(200));
	Durin::FSpotLightSceneData Spot;
	Spot.Position = {0.35, 0.15, 1.0};
	Spot.Direction = {0.0, 0.0, -1.0};
	Spot.Color = {0.05f, 0.2f, 1.0f};
	Spot.Intensity = 8.0f;
	Spot.Range = 8.0f;
	Spot.InnerConeAngle = 25.0f;
	Spot.OuterConeAngle = 40.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(201), std::make_unique<Durin::FSpotLightSceneProxy>(Spot));
	Durin::FlushRenderingCommands();
	std::vector<std::byte> SpotOnlyForward;
	std::vector<std::byte> SpotOnlyDeferred;
	std::vector<std::byte> SpotOnlySurface;
	CaptureDeferredTerm(SpotOnlyForward, SpotOnlyDeferred, SpotOnlySurface);
	EXPECT_NE(SpotOnlyDeferred, NoLightDeferred);
	EXPECT_NE(SpotOnlyDeferred, PointOnlyDeferred);

	Scene.AddOrReplaceLight(Durin::FLightSceneId(200), std::make_unique<Durin::FPointLightSceneProxy>(Point));
	auto PointTwo = Point;
	PointTwo.Position = {-0.1, -0.45, 0.8};
	PointTwo.Color = {0.1f, 1.0f, 0.2f};
	PointTwo.Intensity = 3.0f;
	PointTwo.Range = 4.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(202), std::make_unique<Durin::FPointLightSceneProxy>(PointTwo));
	auto SpotTwo = Spot;
	SpotTwo.Position = {0.45, -0.35, 1.1};
	SpotTwo.Direction = {0.1, 0.2, -1.0};
	SpotTwo.Color = {1.0f, 0.6f, 0.1f};
	SpotTwo.Intensity = 6.0f;
	SpotTwo.Range = 7.0f;
	SpotTwo.InnerConeAngle = 20.0f;
	SpotTwo.OuterConeAngle = 45.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(203), std::make_unique<Durin::FSpotLightSceneProxy>(SpotTwo));
	auto OverflowPoint = Point;
	OverflowPoint.Color = {1.0f, 0.0f, 1.0f};
	Scene.AddOrReplaceLight(Durin::FLightSceneId(204), std::make_unique<Durin::FPointLightSceneProxy>(OverflowPoint));
	Durin::FlushRenderingCommands();
	std::vector<std::byte> FourLocalForward;
	std::vector<std::byte> FourLocalDeferred;
	std::vector<std::byte> FourLocalSurface;
	CaptureDeferredTerm(FourLocalForward, FourLocalDeferred, FourLocalSurface);
	EXPECT_NE(FourLocalDeferred, PointOnlyDeferred);
	EXPECT_EQ(GLastCounters.SelectedPointLights, 2u);
	EXPECT_EQ(GLastCounters.SelectedSpotLights, 2u);
	EXPECT_EQ(GLastCounters.OverflowPointLights, 1u);
	EXPECT_EQ(GLastCounters.OverflowSpotLights, 0u);

	auto InvalidPoint = Point;
	InvalidPoint.Range = 0.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(199), std::make_unique<Durin::FPointLightSceneProxy>(InvalidPoint));
	Durin::FlushRenderingCommands();
	std::vector<std::byte> InvalidLocalForward;
	std::vector<std::byte> InvalidLocalDeferred;
	std::vector<std::byte> InvalidLocalSurface;
	CaptureDeferredTerm(
		InvalidLocalForward, InvalidLocalDeferred, InvalidLocalSurface
	);
	EXPECT_EQ(InvalidLocalForward, FourLocalForward);
	EXPECT_EQ(InvalidLocalDeferred, FourLocalDeferred);
	EXPECT_EQ(GLastCounters.RejectedPointLights, 1u);

	std::vector<std::byte> LocalDiagnosticOutput;
	std::vector<std::byte> LocalDiagnosticHDR;
	const Durin::FViewRenderCounters LocalDiagnosticCounters = RenderCapture(
		false, false, LocalDiagnosticOutput, false, nullptr, nullptr,
		false, nullptr, nullptr, Durin::EGBufferDebugMode::Disabled,
		nullptr, nullptr, Durin::ERenderMode::Lit, true,
		Durin::EDeferredDirectionalDebugMode::Local, &LocalDiagnosticHDR
	);
	EXPECT_EQ(LocalDiagnosticCounters.DeferredDirectionalDebugViews, 1u);
	EXPECT_EQ(LocalDiagnosticCounters.DeferredDirectionalPassFailures, 0u);
	EXPECT_TRUE(std::ranges::any_of(LocalDiagnosticHDR, [](uint8 Value) { return Value != 0u; }));
	// This fixture has no directional, environment, or emissive term, so the
	// isolated local component is exactly the final deferred result.
	EXPECT_EQ(LocalDiagnosticHDR, InvalidLocalDeferred);

	for (const uint64 Id : {199u, 200u, 201u, 202u, 203u, 204u})
		Scene.RemoveLight(Durin::FLightSceneId(Id));

	Directional.bCastShadows = true;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(100), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	SetFixtureMaterial(Opaque);
	Durin::FlushRenderingCommands();
	std::vector<std::byte> ContactForwardOutput;
	std::vector<std::byte> ContactForwardHDR;
	RenderCapture(true, false, ContactForwardOutput, false, &ContactForwardHDR);
	std::vector<std::byte> ContactHybridOutput;
	std::vector<std::byte> ContactHybridHDR;
	std::vector<std::byte> ContactHybridSurface;
	const Durin::FViewRenderCounters ContactHybridCounters = RenderCapture(
		true, false, ContactHybridOutput, false, &ContactHybridHDR, nullptr,
		false, nullptr, &ContactHybridSurface,
		Durin::EGBufferDebugMode::Disabled, nullptr, nullptr,
		Durin::ERenderMode::Lit, false,
		Durin::EDeferredDirectionalDebugMode::Disabled, nullptr,
		Durin::EDirectionalShadowCandidate::SingleMap,
		Durin::EDirectionalShadowFilterQuality::Low,
		Durin::EDirectionalShadowDiagnosticMode::Lit, 0.0f
	);
	std::vector<std::byte> ContactFragmentOutput;
	std::vector<std::byte> ContactFragmentHDR;
	const Durin::FViewRenderCounters ContactFragmentCounters = RenderCapture(
		true, false, ContactFragmentOutput, false, &ContactFragmentHDR, nullptr,
		false, nullptr, nullptr, Durin::EGBufferDebugMode::Disabled,
		nullptr, nullptr, Durin::ERenderMode::Lit, false,
		Durin::EDeferredDirectionalDebugMode::Disabled, nullptr,
		Durin::EDirectionalShadowCandidate::SingleMap,
		Durin::EDirectionalShadowFilterQuality::Low,
		Durin::EDirectionalShadowDiagnosticMode::Lit, 0.0f, true);
	EXPECT_EQ(ContactFragmentOutput, ContactHybridOutput);
	EXPECT_EQ(ContactFragmentHDR, ContactHybridHDR);
	ExpectDeferredParity(ContactForwardHDR, ContactHybridHDR, ContactHybridSurface, "production-contact-shadow");
	EXPECT_EQ(ContactHybridCounters.HybridDeferredEnabledViews, 1u);
	EXPECT_EQ(ContactHybridCounters.ContactShadowEnabledViews, 1u);
	EXPECT_EQ(ContactHybridCounters.ContactShadowPassFailures, 0u);
	EXPECT_EQ(ContactHybridCounters.ContactShadowComputeViews, 1u);
	EXPECT_EQ(ContactHybridCounters.ContactShadowFragmentViews, 0u);
	EXPECT_EQ(ContactHybridCounters.ContactShadowDispatches, 1u);
	EXPECT_EQ(ContactHybridCounters.ContactShadowDraws, 0u);
	EXPECT_EQ(ContactHybridCounters.ContactShadowActiveBytes,
		Durin::FContactShadowVisibilityRenderer::CalculateTargetBytes(
			CaptureWidth, CaptureHeight));
	EXPECT_EQ(ContactHybridCounters.ContactShadowRetainedBytes,
		2u * Durin::FContactShadowVisibilityRenderer::CalculateTargetBytes(
			CaptureWidth, CaptureHeight));
	EXPECT_EQ(ContactFragmentCounters.ContactShadowComputeViews, 0u);
	EXPECT_EQ(ContactFragmentCounters.ContactShadowFragmentViews, 1u);
	EXPECT_EQ(ContactFragmentCounters.ContactShadowDispatches, 0u);
	EXPECT_EQ(ContactFragmentCounters.ContactShadowDraws, 1u);

	// The pass must run exactly once when enabled, zero when disabled, and
	// never report a resource/input failure.
	EXPECT_EQ(CountersOff.ContactShadowEnabledViews, 0u);
	EXPECT_EQ(CountersOff.ContactShadowDispatches, 0u);
	EXPECT_EQ(CountersOff.ContactShadowDraws, 0u);
	EXPECT_EQ(CountersOn.ContactShadowEnabledViews, 1u);
	EXPECT_EQ(CountersOn.ContactShadowPassFailures, 0u);
	EXPECT_EQ(CountersOn.ContactShadowComputeViews, 1u);
	EXPECT_EQ(CountersOn.ContactShadowDispatches, 1u);
	EXPECT_EQ(CountersOn.ContactShadowDraws, 0u);
	EXPECT_EQ(GBufferCounters.GBufferEnabledViews, 1u);
	EXPECT_EQ(GBufferCounters.GBufferUnavailableViews, 0u);
	EXPECT_EQ(GBufferCounters.GBufferAttachmentBytes, Durin::FGBufferRenderer::CalculateTargetBytes(CaptureWidth, CaptureHeight));
	EXPECT_EQ(GBufferCounters.GBufferAttemptedDraws, 3u);
	EXPECT_EQ(GBufferCounters.GBufferSuccessfulDraws, 3u);
	EXPECT_EQ(GBufferCounters.GBufferRejectedDraws, 0u);
	EXPECT_EQ(GBufferCounters.GBufferSkippedDraws, 0u);
	EXPECT_EQ(GBufferCounters.GBufferStaticMeshAttemptedDraws, 3u);
	EXPECT_EQ(GBufferCounters.GBufferStaticMeshSuccessfulDraws, 3u);
	EXPECT_EQ(GBufferCounters.GBufferStaticMeshRejectedDraws, 0u);
	EXPECT_EQ(GBufferCounters.GBufferStaticMeshSkippedDraws, 0u);
	EXPECT_EQ(GBufferCounters.GBufferSplineMeshAttemptedDraws, 0u);
	EXPECT_EQ(GBufferCounters.GBufferSkeletalMeshAttemptedDraws, 0u);
	EXPECT_EQ(GBufferCounters.GBufferTerrainAttemptedDraws, 0u);
	EXPECT_EQ(GBufferPixels, PixelsOff);
	ASSERT_EQ(GBufferMaterialPixels.size(), static_cast<size_t>(CaptureWidth) * CaptureHeight * 4u);
	ASSERT_EQ(GBufferSurfacePixels.size(), GBufferMaterialPixels.size());
	ASSERT_EQ(GBufferNormalsPixels.size(), GBufferMaterialPixels.size());
	ASSERT_EQ(GBufferEmissivePixels.size(), GBufferMaterialPixels.size());
	size_t ValidGBufferPixels = 0;
	size_t BackgroundGBufferPixels = 0;
	for (size_t Offset = 0; Offset < GBufferSurfacePixels.size(); Offset += 4)
	{
		uint32 PackedEmissive = 0;
		std::memcpy(&PackedEmissive, GBufferEmissivePixels.data() + Offset, sizeof(PackedEmissive));
		if (GBufferSurfacePixels[Offset + 3] == 0u)
		{
			++BackgroundGBufferPixels;
			EXPECT_EQ(GBufferMaterialPixels[Offset], 0u);
			EXPECT_EQ(GBufferMaterialPixels[Offset + 1], 0u);
			EXPECT_EQ(GBufferMaterialPixels[Offset + 2], 0u);
			EXPECT_EQ(GBufferMaterialPixels[Offset + 3], 0u);
			EXPECT_EQ(GBufferNormalsPixels[Offset], 0u);
			EXPECT_EQ(GBufferNormalsPixels[Offset + 1], 0u);
			EXPECT_EQ(GBufferNormalsPixels[Offset + 2], 0u);
			EXPECT_EQ(GBufferNormalsPixels[Offset + 3], 0u);
			EXPECT_EQ(PackedEmissive, 0u);
			continue;
		}
		++ValidGBufferPixels;
		const auto ToUNorm = [](uint8 Value) {
			return static_cast<float>(Value) / 255.0f;
		};
		const Durin::GBufferContract::FDecodedRecord Record =
			Durin::GBufferContract::DecodeRecord(
				{ToUNorm(GBufferMaterialPixels[Offset]),
				 ToUNorm(GBufferMaterialPixels[Offset + 1]),
				 ToUNorm(GBufferMaterialPixels[Offset + 2]),
				 ToUNorm(GBufferMaterialPixels[Offset + 3])},
				{ToUNorm(GBufferNormalsPixels[Offset]),
				 ToUNorm(GBufferNormalsPixels[Offset + 1]),
				 ToUNorm(GBufferNormalsPixels[Offset + 2]),
				 ToUNorm(GBufferNormalsPixels[Offset + 3])},
				{ToUNorm(GBufferSurfacePixels[Offset]),
				 ToUNorm(GBufferSurfacePixels[Offset + 1]),
				 ToUNorm(GBufferSurfacePixels[Offset + 2]),
				 ToUNorm(GBufferSurfacePixels[Offset + 3])},
				Durin::GBufferContract::DecodeR11G11B10Float(
					PackedEmissive
				)
			);
		EXPECT_EQ(Record.Flags, Durin::GBufferContract::StandardLitFlag);
		EXPECT_TRUE(Record.IsStandardLit());
		for (const float Channel : {Record.BaseColor.x, Record.BaseColor.y, Record.BaseColor.z})
		{
			EXPECT_NEAR(Channel, 0.72f, Durin::GBufferContract::MaximumUNorm8Error);
		}
		EXPECT_NEAR(Record.Metallic, 0.0f, Durin::GBufferContract::MaximumUNorm8Error);
		EXPECT_NEAR(Record.Roughness, 0.5f, Durin::GBufferContract::MaximumUNorm8Error);
		EXPECT_NEAR(Record.AmbientOcclusion, 1.0f, Durin::GBufferContract::MaximumUNorm8Error);
		EXPECT_NEAR(Record.EffectiveOpacity, 1.0f, Durin::GBufferContract::MaximumUNorm8Error);
		EXPECT_NEAR(Record.Emissive.x, 0.0f, 0.00006104f);
		EXPECT_NEAR(Record.Emissive.y, 0.0f, 0.00006104f);
		EXPECT_NEAR(Record.Emissive.z, 1.1f, 0.011f);
		EXPECT_NEAR(Durin::Math::Length(Record.ShadingNormal), 1.0, 1.0e-5);
		EXPECT_NEAR(Durin::Math::Length(Record.GeometricNormal), 1.0, 1.0e-5);
	}
	EXPECT_GT(ValidGBufferPixels, 0u);
	EXPECT_GT(BackgroundGBufferPixels, 0u);
	const std::array GBufferDebugModes{
		Durin::EGBufferDebugMode::Material,
		Durin::EGBufferDebugMode::ShadingNormal,
		Durin::EGBufferDebugMode::GeometricNormal,
		Durin::EGBufferDebugMode::Surface,
		Durin::EGBufferDebugMode::Emissive,
		Durin::EGBufferDebugMode::Flags,
		Durin::EGBufferDebugMode::Depth,
		Durin::EGBufferDebugMode::ViewPosition,
		Durin::EGBufferDebugMode::ReconstructionError
	};
	std::vector<std::vector<std::byte>> GBufferDebugImages;
	for (const Durin::EGBufferDebugMode Mode : GBufferDebugModes)
	{
		auto& Image = GBufferDebugImages.emplace_back();
		const Durin::FViewRenderCounters DebugViewCounters = RenderCapture(
			false, false, Image, false, nullptr, nullptr, false,
			nullptr, nullptr, Mode
		);
		EXPECT_EQ(DebugViewCounters.GBufferEnabledViews, 1u);
		EXPECT_EQ(DebugViewCounters.GBufferDebugViews, 1u);
		EXPECT_EQ(DebugViewCounters.GBufferDebugFailures, 0u);
		EXPECT_NE(Image, PixelsOff);
		EXPECT_TRUE(std::ranges::any_of(Image, [](uint8 Value) {
			return Value != 0u;
		}));
	}
	EXPECT_NE(GBufferDebugImages.front(), GBufferDebugImages.back());
	size_t SampledDepthPixels = 0;
	for (size_t Offset = 0; Offset < GBufferDebugImages[6].size(); Offset += 4)
	{
		if (GBufferDebugImages[6][Offset] > 0u
			&& GBufferDebugImages[6][Offset] < 255u)
		{
			++SampledDepthPixels;
			EXPECT_EQ(GBufferDebugImages[6][Offset], GBufferDebugImages[6][Offset + 1]);
			EXPECT_EQ(GBufferDebugImages[6][Offset + 1], GBufferDebugImages[6][Offset + 2]);
		}
	}
	EXPECT_GT(SampledDepthPixels, 0u);
	size_t WithinTolerancePixels = 0;
	for (size_t Offset = 0; Offset < GBufferDebugImages.back().size(); Offset += 4)
	{
		if (GBufferDebugImages.back()[Offset + 1]
			> GBufferDebugImages.back()[Offset])
			++WithinTolerancePixels;
	}
	EXPECT_GT(WithinTolerancePixels, 0u);
	std::vector<std::byte> ForwardMaterialInputs;
	RenderCapture(false, false, ForwardMaterialInputs, false, nullptr, nullptr, false, nullptr, nullptr, Durin::EGBufferDebugMode::Disabled, nullptr, nullptr, Durin::ERenderMode::Unlit);
	std::vector<std::byte> DecodedMaterialInputs;
	const Durin::FViewRenderCounters MaterialABCounters = RenderCapture(
		false, false, DecodedMaterialInputs, false,
		nullptr, nullptr, false, nullptr, nullptr,
		Durin::EGBufferDebugMode::MaterialInputs, nullptr, nullptr,
		Durin::ERenderMode::Unlit
	);
	EXPECT_EQ(MaterialABCounters.GBufferDebugViews, 1u);
	ASSERT_EQ(DecodedMaterialInputs.size(), ForwardMaterialInputs.size());
	for (size_t Offset = 0; Offset < DecodedMaterialInputs.size(); ++Offset)
	{
		EXPECT_LE(std::abs(static_cast<int>(DecodedMaterialInputs[Offset]) - static_cast<int>(ForwardMaterialInputs[Offset])), 2);
	}
	const size_t ExpectedHDRBytes =
		static_cast<size_t>(CaptureWidth) * CaptureHeight * 8u;
	EXPECT_EQ(HDRSceneOff.size(), ExpectedHDRBytes);
	EXPECT_EQ(HDRInputOff, HDRSceneOff);
	EXPECT_EQ(HDRSceneOn.size(), ExpectedHDRBytes);
	EXPECT_EQ(HDRInputOn.size(), ExpectedHDRBytes);
	EXPECT_NE(HDRSceneOn, HDRSceneOff);
	EXPECT_EQ(HDRInputOn, HDRSceneOn);
	auto ContainsHalfAboveOne = [](const std::vector<std::byte>& Pixels) {
		for (size_t Offset = 0; Offset + 7 < Pixels.size(); Offset += 8)
		{
			for (size_t Channel = 0; Channel < 3; ++Channel)
			{
				const size_t ChannelOffset = Offset + Channel * 2;
				const uint16 Bits =
					static_cast<uint16>(Pixels[ChannelOffset])
					| static_cast<uint16>(
						Pixels[ChannelOffset + 1] << 8
					);
				if (Bits > 0x3c00u && Bits < 0x7c00u) return true;
			}
		}
		return false;
	};
	EXPECT_TRUE(ContainsHalfAboveOne(HDRSceneOff));

	// The enabled image must differ from the disabled one and the difference
	// must stay bounded (near-field contact, not a whole-frame darkening).
	EXPECT_NE(PixelsOn, PixelsOff);
	const size_t ChangedPixels = CountChangedPixels(PixelsOn, PixelsOff, 2);
	EXPECT_GT(ChangedPixels, 0u);
	EXPECT_LT(ChangedPixels, CaptureWidth * CaptureHeight / 2u);

	// The editor's contact-contribution diagnostic is a bounded red mask, not
	// another lighting mode. Keep it covered by the same near-field fixture.
	std::vector<std::byte> DebugPixels;
	const Durin::FViewRenderCounters DebugCounters =
		RenderCapture(true, true, DebugPixels);
	std::vector<std::byte> FragmentDebugPixels;
	RenderCapture(true, true, FragmentDebugPixels, false, nullptr, nullptr,
		false, nullptr, nullptr, Durin::EGBufferDebugMode::Disabled,
		nullptr, nullptr, Durin::ERenderMode::Lit, false,
		Durin::EDeferredDirectionalDebugMode::Disabled, nullptr,
		Durin::EDirectionalShadowCandidate::SingleMap,
		Durin::EDirectionalShadowFilterQuality::Low,
		Durin::EDirectionalShadowDiagnosticMode::Lit, 0.0f, true);
	EXPECT_EQ(FragmentDebugPixels, DebugPixels);
	EXPECT_EQ(DebugCounters.ContactShadowEnabledViews, 1u);
	EXPECT_EQ(DebugCounters.ContactShadowPassFailures, 0u);
	size_t DebugContributionPixels = 0;
	for (size_t Pixel = 0; Pixel + 3 < DebugPixels.size(); Pixel += 4)
	{
		if (DebugPixels[Pixel] > 2)
		{
			++DebugContributionPixels;
			EXPECT_LE(DebugPixels[Pixel + 1], 2u);
			EXPECT_LE(DebugPixels[Pixel + 2], 2u);
		}
	}
	EXPECT_GT(DebugContributionPixels, 0u);
	EXPECT_LT(DebugContributionPixels, CaptureWidth * CaptureHeight / 2u);

	// One perspective view complements the orthographic fixture without a
	// costly camera sweep. It catches projection/reconstruction regressions but
	// does not claim view-independent coverage for this screen-space effect.
	std::vector<std::byte> PerspectiveOff;
	std::vector<std::byte> PerspectiveOn;
	RenderCapture(false, false, PerspectiveOff, true);
	const Durin::FViewRenderCounters PerspectiveCounters =
		RenderCapture(true, false, PerspectiveOn, true);
	EXPECT_EQ(PerspectiveCounters.ContactShadowEnabledViews, 1u);
	EXPECT_EQ(PerspectiveCounters.ContactShadowPassFailures, 0u);
	const size_t PerspectiveChangedPixels =
		CountChangedPixels(PerspectiveOn, PerspectiveOff, 2);
	EXPECT_GT(PerspectiveChangedPixels, 0u);
	EXPECT_LT(PerspectiveChangedPixels, CaptureWidth * CaptureHeight / 2u);

	// A parallel blocker only 0.015 world units above the receiver is distinct
	// geometry, not a coplanar self sample. Keep this near-contact range alive
	// while the following fixture rejects the receiver's own triangle planes.
	Scene.AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(2),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1),
		MakeTransform({.Translation = {-0.18, 0.08, -0.485},
			.Scale = {0.22, 0.18, 1.0}})
	);
	Durin::FlushRenderingCommands();
	std::vector<std::byte> CloseContactDebug;
	RenderCapture(true, true, CloseContactDebug);
	size_t CloseContactContributionPixels = 0;
	for (size_t Pixel = 0; Pixel + 3 < CloseContactDebug.size(); Pixel += 4)
		if (CloseContactDebug[Pixel] > 2u)
			++CloseContactContributionPixels;
	EXPECT_GT(CloseContactContributionPixels, 0u);

	// A single tilted receiver is intentionally traced along a shallow outward
	// direction that also moves away from the camera. A point/thickness test
	// re-hits its own two triangles as large wedges; an oriented surface test
	// must leave the receiver completely visible without suppressing the real
	// floating-occluder coverage above.
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(2));
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(3));
	Scene.AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(1),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1),
		MakeTransform({.Translation = {0.0, 0.0, -0.5},
			.Scale = {0.82, 0.82, 1.0}, .RotationYDegrees = 65.0})
	);
	Directional.Direction = {-0.55, 0.0, 0.835};
	Scene.AddOrReplaceLight(
		Durin::FLightSceneId(100),
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional)
	);
	Durin::FlushRenderingCommands();

	// The same shallow receiver must retain a visible contact contribution from
	// independently separated parallel geometry. Directional lighting already
	// carries N.L, so contact visibility must not fade that response a second
	// time merely because the light angle is shallow.
	Scene.AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(2),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1),
		MakeTransform({.Translation = {0.0136, 0.0, -0.4937},
			.Scale = {0.22, 0.18, 1.0}, .RotationYDegrees = 65.0})
	);
	Durin::FlushRenderingCommands();
	std::vector<std::byte> ShallowContactDebug;
	RenderCapture(true, true, ShallowContactDebug);
	uint8 ShallowContactPeak = 0u;
	for (size_t Pixel = 0; Pixel + 3 < ShallowContactDebug.size(); Pixel += 4)
		ShallowContactPeak = std::max(
			ShallowContactPeak, ShallowContactDebug[Pixel]);
	EXPECT_GT(ShallowContactPeak, 96u);

	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(2));
	Durin::FlushRenderingCommands();
	std::vector<std::byte> CoplanarOff;
	std::vector<std::byte> CoplanarOn;
	RenderCapture(false, false, CoplanarOff);
	const Durin::FViewRenderCounters CoplanarCounters =
		RenderCapture(true, false, CoplanarOn);
	EXPECT_EQ(CoplanarCounters.ContactShadowEnabledViews, 1u);
	EXPECT_EQ(CoplanarCounters.ContactShadowPassFailures, 0u);
	EXPECT_EQ(CoplanarOn, CoplanarOff);

	const FCaptureStatistics StatsOff =
		CalculateStatistics("contact_off", PixelsOff, CountersOff);
	const FCaptureStatistics StatsOn =
		CalculateStatistics("contact_on", PixelsOn, CountersOn);
	const std::filesystem::path OutputDirectory =
		Durin::Testing::CreateTestFixtureDirectory(
			"DirectionalContactShadow"
		);
	WritePpm(OutputDirectory / "contact_shadow_off.ppm", PixelsOff);
	WritePpm(OutputDirectory / "contact_shadow_on.ppm", PixelsOn);
	std::cout << "Contact shadow changed pixels: " << ChangedPixels
			  << " (off dark/mid/bright " << StatsOff.DarkPixels << "/"
			  << StatsOff.MidPixels << "/" << StatsOff.BrightPixels
			  << ", on " << StatsOn.DarkPixels << "/" << StatsOn.MidPixels << "/"
			  << StatsOn.BrightPixels << ")\n";

	// A production Unlit material with authored emissive radiance above one
	// must remain unclipped in Scene Color before the display transform.
	auto Emissive = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque,
		{0.0, 0.0, 0.0},
		Durin::EMaterialShadingModel::Unlit,
		{4.0, 2.0, 0.5}
	);
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(1), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Emissive}, 1), MakeTransform({.Translation = {0.0, 0.0, -0.5}, .Scale = {0.82, 0.82, 1.0}}));
	Durin::FlushRenderingCommands();
	std::vector<std::byte> EmissiveOutput;
	std::vector<std::byte> EmissiveHDRScene;
	std::vector<std::byte> EmissiveHDRInput;
	RenderCapture(false, false, EmissiveOutput, false, &EmissiveHDRScene, &EmissiveHDRInput);
	EXPECT_TRUE(ContainsHalfAboveOne(EmissiveHDRScene));
	EXPECT_EQ(EmissiveHDRInput, EmissiveHDRScene);

	// Contact shadows may only attenuate the selected directional direct term.
	// Preserve the same depth and occluder configuration while rendering it
	// unlit: no deferred receiver exists, so no contact pass is recorded.
	auto Unlit = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque,
		{0.35, 0.22, 0.12},
		Durin::EMaterialShadingModel::Unlit
	);
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(1), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Unlit}, 1), MakeTransform({.Translation = {0.0, 0.0, -0.5}, .Scale = {0.82, 0.82, 1.0}}));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Unlit}, 1), MakeTransform({.Translation = {-0.18, 0.08, -0.4}, .Scale = {0.22, 0.18, 1.0}}));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(3), std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Unlit}, 1), MakeTransform({.Translation = {-0.35, 0.0, -0.45}, .Scale = {0.25, 0.25, 1.0}, .RotationYDegrees = 90.0}));
	Durin::FlushRenderingCommands();
	std::vector<std::byte> UnlitOff;
	std::vector<std::byte> UnlitOn;
	std::vector<std::byte> UnlitDeferredHDR;
	RenderCapture(false, false, UnlitOff);
	const Durin::FViewRenderCounters UnlitCounters =
		RenderCapture(true, false, UnlitOn, false, nullptr, nullptr, false, nullptr, nullptr, Durin::EGBufferDebugMode::Disabled, nullptr, nullptr, Durin::ERenderMode::Lit, true, Durin::EDeferredDirectionalDebugMode::Final, &UnlitDeferredHDR);
	EXPECT_EQ(UnlitCounters.ContactShadowEnabledViews, 0u);
	EXPECT_EQ(UnlitCounters.ContactShadowPassFailures, 0u);
	EXPECT_EQ(UnlitCounters.GBufferAttemptedDraws, 0u);
	EXPECT_EQ(UnlitCounters.DeferredDirectionalEnabledViews, 1u);
	EXPECT_EQ(UnlitCounters.DeferredDirectionalPassFailures, 0u);
	EXPECT_EQ(UnlitOn, UnlitOff);
	ASSERT_EQ(UnlitDeferredHDR.size(), static_cast<size_t>(CaptureWidth) * CaptureHeight * 8u);
	for (size_t Offset = 8; Offset < UnlitDeferredHDR.size(); Offset += 8)
	{
		EXPECT_EQ(std::memcmp(UnlitDeferredHDR.data(), UnlitDeferredHDR.data() + Offset, 8), 0);
	}

	Durin::SetViewRenderCounterSink(nullptr);
	RendererLifecycle.Shutdown();
	Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
		[&](Durin::FRHICommandListImmediate&) { Quad->ReleaseResources(); }
	);
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
