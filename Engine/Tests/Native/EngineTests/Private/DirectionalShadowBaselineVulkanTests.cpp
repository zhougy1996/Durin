#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialRenderProxy.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestContext.h"
#include "NativeTestSupport.h"
#include "RHICommandList.h"
#include "RendererModule.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/SceneVisibility.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "StaticMesh/StaticMeshResources.h"

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <complex>
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
	constexpr Durin::uint32 CaptureWidth = 257;
	constexpr Durin::uint32 CaptureHeight = 257;

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
		std::array<Durin::uint8, 4> Minimum{255, 255, 255, 255};
		std::array<Durin::uint8, 4> Maximum{0, 0, 0, 0};
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

	auto CaptureCounters(const Durin::FViewRenderCounters& Counters) -> void
	{
		GLastCounters = Counters;
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
			{-1.0f, 1.0f, 0.0f}};
		LOD.VertexBuffers.PositionVertexBuffer.Init(Positions);
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(4, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(4, {1.0f, 0.0f, 0.0f, 1.0f}));
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels>
			UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), 4, 1);
		LOD.VertexBuffers.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(4, Durin::FVector4f(1.0f)), 4);
		LOD.IndexBuffer.Init({0, 1, 2, 0, 2, 3});
		LOD.Sections.push_back({
			.Name = "Surface",
			.FirstIndex = 0,
			.IndexCount = 6,
			.MinVertexIndex = 0,
			.MaxVertexIndex = 3,
			.MaterialSlotIndex = 0,
			.LocalBounds = Durin::FBox({-1.0, -1.0, 0.0}, {1.0, 1.0, 0.0})});
		LOD.LocalBounds = LOD.Sections[0].LocalBounds;
		LOD.NumTexCoords = 1;
		LOD.bHasColorVertexData = true;
		Data->LODVertexFactories.resize(1);
		Data->RecalculateBounds();
		return Data;
	}

	auto MakeMaterial(Durin::EMaterialBlendMode BlendMode,
		const Durin::FVector3& BaseColor,
		Durin::EMaterialShadingModel ShadingModel =
			Durin::EMaterialShadingModel::Lit) -> Durin::FMaterialRenderProxyRef
	{
		auto Proxy = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
		Durin::FMaterialRenderProxyPublication Publication;
		Publication.LocalVersion = 1;
		Publication.LocalLayer.StaticProperties = Durin::FMaterialStaticProperties{
			.BlendMode = BlendMode,
			.ShadingModel = ShadingModel,
			.bTwoSided = true,
			.OpacityMaskThreshold = 0.4f};
		Publication.LocalLayer.Parameters.push_back({
			.Id = Durin::MaterialParameters::BaseColorId,
			.Type = Durin::EMaterialParameterType::Vector,
			.VectorValue = BaseColor});
		if (BlendMode == Durin::EMaterialBlendMode::Masked)
		{
			Publication.LocalLayer.Parameters.push_back({
				.Id = Durin::MaterialParameters::OpacityMaskId,
				.Type = Durin::EMaterialParameterType::Scalar,
				.ScalarValue = 1.0f});
		}
		EXPECT_TRUE(Proxy->QueuePublication_GameThread(std::move(Publication)));
		return Proxy;
	}

	auto MakeTransform(const FPrimitivePlacement& Placement) -> Durin::FMatrix
	{
		Durin::FMatrix Transform = glm::translate(
			Durin::FMatrix(1.0), Placement.Translation);
		if (Placement.RotationYDegrees != 0.0)
		{
			Transform = glm::rotate(Transform,
				glm::radians(Placement.RotationYDegrees),
				Durin::FVector3(0.0, 1.0, 0.0));
		}
		if (Placement.RotationZDegrees != 0.0)
		{
			Transform = glm::rotate(Transform,
				glm::radians(Placement.RotationZDegrees),
				Durin::FVector3(0.0, 0.0, 1.0));
		}
		return glm::scale(Transform, Placement.Scale);
	}

	auto CalculateStatistics(std::string Name,
		const std::vector<Durin::uint8>& Pixels,
		const Durin::FViewRenderCounters& Counters) -> FCaptureStatistics
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
					+ static_cast<unsigned>(Pixels[Offset + 2]) * 19u) / 256u;
			Result.DarkPixels += Luminance < 48u ? 1u : 0u;
			Result.MidPixels += Luminance >= 48u && Luminance < 160u ? 1u : 0u;
			Result.BrightPixels += Luminance >= 160u ? 1u : 0u;
			for (size_t Channel = 0; Channel < 4; ++Channel)
			{
				Result.Minimum[Channel] = std::min(
					Result.Minimum[Channel], Pixels[Offset + Channel]);
				Result.Maximum[Channel] = std::max(
					Result.Maximum[Channel], Pixels[Offset + Channel]);
				Result.Mean[Channel] += Pixels[Offset + Channel];
			}
		}
		const double PixelCount = static_cast<double>(Pixels.size() / 4u);
		for (double& Value : Result.Mean) Value /= PixelCount;
		return Result;
	}

	auto CountChangedPixels(const std::vector<Durin::uint8>& First,
		const std::vector<Durin::uint8>& Second, Durin::uint8 ChannelTolerance)
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

	auto LuminanceAt(const std::vector<Durin::uint8>& Pixels,
		Durin::uint32 X, Durin::uint32 Y) -> int
	{
		const size_t Offset = (static_cast<size_t>(Y) * CaptureWidth + X) * 4u;
		return static_cast<int>((static_cast<unsigned>(Pixels[Offset]) * 54u
			+ static_cast<unsigned>(Pixels[Offset + 1]) * 183u
			+ static_cast<unsigned>(Pixels[Offset + 2]) * 19u) / 256u);
	}

	auto MaximumTransitionWidth(
		const std::vector<Durin::uint8>& Pixels) -> size_t
	{
		size_t Maximum = 0;
		auto MeasureLine = [&](bool bVertical, Durin::uint32 Fixed) {
			size_t Run = 0;
			for (Durin::uint32 Variable = 0; Variable < CaptureWidth; ++Variable)
			{
				const Durin::uint32 X = bVertical ? Fixed : Variable;
				const Durin::uint32 Y = bVertical ? Variable : Fixed;
				const int Luminance = LuminanceAt(Pixels, X, Y);
				Run = Luminance >= 48 && Luminance < 160 ? Run + 1u : 0u;
				Maximum = std::max(Maximum, Run);
			}
		};
		for (Durin::uint32 Line = 0; Line < CaptureWidth; ++Line)
		{
			MeasureLine(false, Line);
			MeasureLine(true, Line);
		}
		return Maximum;
	}

	auto ShadowDifferenceAt(
		const std::vector<Durin::uint8>& Enabled,
		const std::vector<Durin::uint8>& Disabled,
		Durin::uint32 X, Durin::uint32 Y) -> int
	{
		const int Reference = LuminanceAt(Disabled, X, Y);
		return Reference >= 160
			? std::max(Reference - LuminanceAt(Enabled, X, Y), 0) : 0;
	}

	auto TransformRadix2(std::complex<double>* Values, size_t Count) -> void
	{
		for (size_t Index = 1, Reversed = 0; Index < Count; ++Index)
		{
			size_t Bit = Count >> 1u;
			for (; (Reversed & Bit) != 0; Bit >>= 1u) Reversed ^= Bit;
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
		const std::vector<Durin::uint8>& Enabled,
		const std::vector<Durin::uint8>& Disabled) -> double
	{
		constexpr size_t TransformSize = 256;
		constexpr size_t RoiMinimum = 32;
		constexpr size_t RoiMaximum = 225;
		constexpr size_t RoiSize = RoiMaximum - RoiMinimum;
		std::vector<std::complex<double>> Values(
			TransformSize * TransformSize);
		double Mean = 0.0;
		for (size_t Y = RoiMinimum; Y < RoiMaximum; ++Y)
			for (size_t X = RoiMinimum; X < RoiMaximum; ++X)
				Mean += ShadowDifferenceAt(Enabled, Disabled,
					static_cast<Durin::uint32>(X), static_cast<Durin::uint32>(Y));
		Mean /= static_cast<double>(RoiSize * RoiSize);
		for (size_t Y = 0; Y < RoiSize; ++Y)
			for (size_t X = 0; X < RoiSize; ++X)
				Values[Y * TransformSize + X] =
					static_cast<double>(ShadowDifferenceAt(Enabled, Disabled,
						static_cast<Durin::uint32>(X + RoiMinimum),
						static_cast<Durin::uint32>(Y + RoiMinimum))) - Mean;
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
					X <= TransformSize / 2u ? X : TransformSize - X);
				const double FrequencyY = static_cast<double>(
					Y <= TransformSize / 2u ? Y : TransformSize - Y);
				const double Energy = std::norm(Values[Y * TransformSize + X]);
				TotalEnergy += Energy;
				if (std::sqrt(FrequencyX * FrequencyX + FrequencyY * FrequencyY)
					/ static_cast<double>(TransformSize) > 0.30)
					HighFrequencyEnergy += Energy;
			}
		return TotalEnergy > 0.0 ? HighFrequencyEnergy / TotalEnergy : 0.0;
	}


	auto WritePpm(const std::filesystem::path& Path,
		const std::vector<Durin::uint8>& Pixels) -> void
	{
		std::ofstream Stream(Path, std::ios::binary);
		ASSERT_TRUE(Stream.is_open()) << Path.string();
		Stream << "P6\n" << CaptureWidth << ' ' << CaptureHeight << "\n255\n";
		for (size_t Offset = 0; Offset + 3 < Pixels.size(); Offset += 4)
		{
			Stream.write(reinterpret_cast<const char*>(Pixels.data() + Offset), 3);
		}
		ASSERT_TRUE(Stream.good()) << Path.string();
	}

	auto WriteMetrics(const std::filesystem::path& Path,
		const std::vector<FCaptureStatistics>& Statistics,
		const std::array<size_t, 3>& MotionChangedPixels,
		const std::array<size_t, 2>& Q1EntryMotionChangedPixels,
		const std::array<double, 3>& ShadowOnlyHighFrequencyFraction,
		const std::array<size_t, 2>& MediumMotion,
		const std::array<size_t, 2>& HighMotion) -> void
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

TEST(FDirectionalShadowBaselineVulkanTests,
	CapturesFrozenLitArtifactsAndSubTexelMotion)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	Durin::FRendererModule Renderer;
	auto RendererContext = Durin::FModuleTestContextFactory::CreateStartupContext("DirectionalShadowRendererTest");
	Renderer.StartupModule(RendererContext);
	Durin::SetViewRenderCounterSink(CaptureCounters);

	auto Quad = MakeQuadRenderData();
	Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(Quad->InitResources(CommandList));
		});
	Durin::FlushRenderingCommands();
	auto Opaque = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque, {0.72, 0.72, 0.72});
	auto Masked = MakeMaterial(
		Durin::EMaterialBlendMode::Masked, {0.72, 0.72, 0.72});

	std::vector<FFixture> Fixtures{
		{"q0_planar_acne_valid",
			{{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.82, 0.82, 1.0}}}},
		{"q0_sloped_acne_valid",
			{{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.75, 0.75, 1.0},
				.RotationYDegrees = 28.0}}},
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
			 {.Translation = {-0.18, 0.08, 0.65}, .Scale = {0.22, 0.18, 1.0},
				.bMasked = true}}},
		{"q0_opaque_coverage_reference",
			{{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.82, 0.82, 1.0}},
			 {.Translation = {-0.18, 0.08, 0.65}, .Scale = {0.22, 0.18, 1.0}}}},
		{"q0_shadow_disabled_reference",
			{{.Translation = {0.0, 0.0, 0.4}, .Scale = {0.82, 0.82, 1.0}},
			 {.Translation = {-0.18, 0.08, 0.52}, .Scale = {0.22, 0.18, 1.0}}},
			{0.35, 0.2, -1.0}, 0.0, false},
		{"q0_motion_translate_00",
			{{.Translation = {-0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
			 {.Translation = {0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
			 {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}}},
		{"q0_motion_translate_01",
			{{.Translation = {-0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
			 {.Translation = {0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
			 {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}},
			{0.35, 0.2, -1.0}, 0.000244140625},
		{"q0_motion_translate_02",
			{{.Translation = {-0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
			 {.Translation = {0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
			 {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}},
			{0.35, 0.2, -1.0}, 0.00048828125},
		{"q0_motion_light_01",
			{{.Translation = {-0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
			 {.Translation = {0.4, 0.0, 0.4}, .Scale = {0.4, 0.8, 1.0}},
			 {.Translation = {0.0, 0.08, 0.65}, .Scale = {0.3, 0.18, 1.0}}},
			{0.3505, 0.2, -1.0}},
		{.Name = "q0_diagnostic_depth_coverage",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::ShadowDepthCoverage},
		{.Name = "q0_diagnostic_receiver_unbiased",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::ReceiverUnbiased},
		{.Name = "q0_diagnostic_receiver_biased",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::ReceiverBiased},
		{.Name = "q0_diagnostic_normal_offset",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::ReceiverNormalOffset},
		{.Name = "q0_diagnostic_texel_grid",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::TexelGrid},
		{.Name = "q0_diagnostic_bias_contributions",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::BiasContributions},
		{.Name = "q0_diagnostic_classification",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::Classification},
		{.Name = "q0_diagnostic_disabled_reference",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .bCastShadows = false,
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::Classification},
		{.Name = "q0_unlit_reference",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .RenderMode = Durin::ERenderMode::Unlit},
		{.Name = "q0_diagnostic_unlit_reference",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.82, 0.82, 1.0}},
			{{-0.18, 0.08, 0.52}, {0.22, 0.18, 1.0}}},
		 .DiagnosticMode = Durin::EDirectionalShadowDiagnosticMode::Classification,
		 .RenderMode = Durin::ERenderMode::Unlit},
		// Q1 entry captures stay after every Q0 index to preserve baseline identity.
		{.Name = "q1_edge_staircase_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}}},
		{.Name = "q1_diagonal_silhouette_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{-0.08, 0.04, 0.64}, {0.46, 0.22, 1.0}, 0.0, 43.0}}},
		{.Name = "q1_thin_caster_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{-0.12, 0.02, 0.64}, {0.018, 0.62, 1.0}, 0.0, 11.0}}},
		{.Name = "q1_masked_cutout_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{-0.28, 0.0, 0.64}, {0.12, 0.55, 1.0}, 0.0, 0.0, true},
			{{0.28, 0.0, 0.64}, {0.12, 0.55, 1.0}, 0.0, 0.0, true},
			{{0.0, 0.36, 0.64}, {0.16, 0.10, 1.0}, 0.0, 0.0, true},
			{{0.0, -0.36, 0.64}, {0.16, 0.10, 1.0}, 0.0, 0.0, true}}},
		{.Name = "q1_guard_boundary_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.98, 0.9, 1.0}},
			{{0.91, 0.0, 0.64}, {0.055, 0.55, 1.0}, 0.0, 7.0}}},
		{.Name = "q1_motion_subpixel_00_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}}},
		{.Name = "q1_motion_subpixel_01_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .CameraTranslationX = 0.0001220703125},
		{.Name = "q1_motion_light_01_low",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .LightDirection = {0.35025, 0.2, -1.0}},
		{.Name = "q1_edge_staircase_shadow_disabled",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .bCastShadows = false},
		{.Name = "q1_edge_staircase_medium",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium},
		{.Name = "q1_motion_subpixel_00_medium",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium},
		{.Name = "q1_motion_subpixel_01_medium",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .CameraTranslationX = 0.0001220703125,
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium},
		{.Name = "q1_motion_light_01_medium",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .LightDirection = {0.35025, 0.2, -1.0},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium},
		{.Name = "q1_edge_staircase_high",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::High},
		{.Name = "q1_motion_subpixel_00_high",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::High},
		{.Name = "q1_motion_subpixel_01_high",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .CameraTranslationX = 0.0001220703125,
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::High},
		{.Name = "q1_motion_light_01_high",
		 .Primitives = {{{0.0, 0.0, 0.4}, {0.9, 0.9, 1.0}},
			{{0.0, 0.0, 0.64}, {0.58, 0.18, 1.0}, 0.0, 17.0}},
		 .LightDirection = {0.35025, 0.2, -1.0},
		 .FilterQuality = Durin::EDirectionalShadowFilterQuality::High}};
	constexpr size_t LowFixtureCount = 31u;
	constexpr std::array<size_t, 21> ParityFixtureIndices{
		0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u,
		23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u};
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
		Durin::EDirectionalShadowFilterQuality::Medium, "_medium");
	const size_t HighParityStart = Fixtures.size();
	AppendTierParity(
		Durin::EDirectionalShadowFilterQuality::High, "_high");
	const size_t FilterDiagnosticStart = Fixtures.size();
	for (const auto [Quality, Suffix] : std::array{
		std::pair{Durin::EDirectionalShadowFilterQuality::Medium,
			std::string_view{"medium"}},
		std::pair{Durin::EDirectionalShadowFilterQuality::High,
			std::string_view{"high"}}})
	{
		for (const auto [Mode, ModeName] : std::array{
			std::pair{Durin::EDirectionalShadowDiagnosticMode::FilterFootprint,
				std::string_view{"footprint"}},
			std::pair{Durin::EDirectionalShadowDiagnosticMode::FilterTapValidity,
				std::string_view{"tap_validity"}},
			std::pair{Durin::EDirectionalShadowDiagnosticMode::FilterDifference,
				std::string_view{"difference"}}})
		{
			FFixture Diagnostic = Fixtures[27u];
			Diagnostic.Name = std::format(
				"q1_diagnostic_{}_{}", ModeName, Suffix);
			Diagnostic.DiagnosticMode = Mode;
			Diagnostic.FilterQuality = Quality;
			Fixtures.push_back(std::move(Diagnostic));
		}
	}
	const size_t CascadeFixtureStart = Fixtures.size();
	Fixtures.push_back({
		.Name = "q2_cascades_index_perspective",
		.Primitives = {
			{{24.0, 0.0, 0.0}, {7.0, 7.0, 1.0}, 90.0},
			{{22.0, 0.5, 0.0}, {2.0, 2.0, 1.0}, 90.0}},
		.LightDirection = {-1.0, 0.2, -0.25},
		.DiagnosticMode =
			Durin::EDirectionalShadowDiagnosticMode::CascadeIndex,
		.FilterQuality = Durin::EDirectionalShadowFilterQuality::Medium,
		.Candidate = Durin::EDirectionalShadowCandidate::ThreeCascades,
		.bPerspective = true});
	constexpr std::array<std::string_view, 13> ExpectedHashes{
		"d2cc1b606edb268b4bcc68a18d624b4d",
		"4d0ac6bb59200618b95c72e76bae8ea1",
		"4716da464b254091bff21a415d29746b",
		"ef4254ba0cc13f8091880e8e8a43009e",
		"7df3b3bbc15a476d7ca4de9d05a5829c",
		"0d107e2f124f290e5baa6025a821c543",
		"f7a7f271bb3021e8ad31e8185f06649f",
		"f7a7f271bb3021e8ad31e8185f06649f",
		"1fa526cb259e748e81027df9fe529c1d",
		"ef4254ba0cc13f8091880e8e8a43009e",
		"5891f7df7f3ad74783af20de9770b0ad",
		"ff0221d5364d85c40017f28c055e9950",
		"61c8ceb1abfc94637e596ac47c0478b4"};
	constexpr std::array<std::string_view, 8> Q1EntryExpectedHashes{
		"65442374e4fc52b725e06e7e9b691a0f",
		"9144c3d82814438293c0afde761a85ce",
		"0a646b78fd9d4663b287810a21b69d0a",
		"6ba21b78c791536aa3262888cc8b451e",
		"e80d091ce711644c6dda8042d75ce63d",
		"65442374e4fc52b725e06e7e9b691a0f",
		"0d8fab92144a4b211e495727535d2805",
		"1e3f0976eb32480d09f4d4323852363a"};
	constexpr std::array<std::string_view, 9> Q1FilterTrialExpectedHashes{
		"9dae114fe1d675583584a4a9a166248b",
		"ac8d347978e0b3f3c7ec91972a481bba",
		"ac8d347978e0b3f3c7ec91972a481bba",
		"bc8987bd55fdee3a1ac13d37f9382fa9",
		"804d5f71bdf4ce3df495866f46b4e90b",
		"7b44b7b2d484552f9166311dcffa1a8d",
		"7b44b7b2d484552f9166311dcffa1a8d",
		"02787f7de37d46f5c02e561e7596dd60",
		"02aa76870a2302c67b8124ab6fdee797"};
	constexpr std::array<std::string_view, 21> MediumParityExpectedHashes{
		"d2cc1b606edb268b4bcc68a18d624b4d",
		"4d0ac6bb59200618b95c72e76bae8ea1",
		"918f30f75e740100eb7284e47d4dd0b3",
		"8d3329bc1f35eba94950ee5a847a225b",
		"a560c34cd242a773860755e1b35a96d6",
		"0d107e2f124f290e5baa6025a821c543",
		"76d05bb24c3c7939a9c4f67a6975a3a6",
		"76d05bb24c3c7939a9c4f67a6975a3a6",
		"1fa526cb259e748e81027df9fe529c1d",
		"8d3329bc1f35eba94950ee5a847a225b",
		"407664acbdc037b7c36429b224a1ff55",
		"b86c2543320ea7e6d415dbf928282929",
		"8615e32c62160dd06675c9f754ca572d",
		"ac8d347978e0b3f3c7ec91972a481bba",
		"d629a304092d939fe15eee90dc497416",
		"796debb8a36fc0189d51646a4643e2bb",
		"3d28a9b1a4c1fef0c72efbbec511d651",
		"b307d0dce5c0be4098b4221c21d57893",
		"ac8d347978e0b3f3c7ec91972a481bba",
		"bc8987bd55fdee3a1ac13d37f9382fa9",
		"804d5f71bdf4ce3df495866f46b4e90b"};
	constexpr std::array<std::string_view, 21> HighParityExpectedHashes{
		"d2cc1b606edb268b4bcc68a18d624b4d",
		"4d0ac6bb59200618b95c72e76bae8ea1",
		"7752648680d3994c0dbb1b0758925828",
		"26ef659c55d60078d373521f530bebbd",
		"73a9763514ea4165d2219311e3f206d3",
		"0d107e2f124f290e5baa6025a821c543",
		"808a725d07ed715b24425da144f7d034",
		"808a725d07ed715b24425da144f7d034",
		"1fa526cb259e748e81027df9fe529c1d",
		"26ef659c55d60078d373521f530bebbd",
		"680f8e5eef73336136c033f9bd499637",
		"7ea836cd60ec15fcf534cebd990ef7f3",
		"f70ccbe2af270b04f83db00f17592be9",
		"7b44b7b2d484552f9166311dcffa1a8d",
		"588bc073eb419a13f0609661d63fc86c",
		"0277328a81c9696b1fe7711ecdb85017",
		"869b2dea1ce9f0e4f7e594ccaa25d32f",
		"b2e7368a90842f4a2fab0b14f30a0a08",
		"7b44b7b2d484552f9166311dcffa1a8d",
		"02787f7de37d46f5c02e561e7596dd60",
		"02aa76870a2302c67b8124ab6fdee797"};
	constexpr std::array<std::string_view, 6> FilterDiagnosticExpectedHashes{
		"2b163cee21149cc5e917b9aaa03ec7cd",
		"3ff57fab7e38ea692b4c8d5b95bb3c1e",
		"f85d64a0139c37dceb83d47d94a4acad",
		"50414acd789dfe24723d89c2f702dac7",
		"3ff57fab7e38ea692b4c8d5b95bb3c1e",
		"89b0ea59feaea35995c2ef8330a7fb47"};

	const std::filesystem::path OutputDirectory =
		Durin::Testing::CreateTestFixtureDirectory("DirectionalShadowQ0Baseline");
	std::vector<std::vector<Durin::uint8>> Captures;
	std::vector<FCaptureStatistics> Statistics;
	Captures.reserve(Fixtures.size());
	Statistics.reserve(Fixtures.size());

	for (const FFixture& Fixture : Fixtures)
	{
		Durin::FScene Scene;
		for (size_t Index = 0; Index < Fixture.Primitives.size(); ++Index)
		{
			const FPrimitivePlacement& Placement = Fixture.Primitives[Index];
			Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(Index + 1),
				std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(),
					std::vector<Durin::FMaterialRenderProxyRef>{
						Placement.bMasked ? Masked : Opaque},
					1),
				MakeTransform(Placement));
		}
		Durin::FDirectionalLightSceneData Directional;
		Directional.Direction = Fixture.LightDirection;
		Directional.Color = {1.0f, 1.0f, 1.0f};
		Directional.Intensity = 3.0f;
		Directional.bCastShadows = Fixture.bCastShadows;
		Scene.AddOrReplaceLight(Durin::FLightSceneId(100),
			std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
		Durin::FlushRenderingCommands();

		auto Pixels = std::make_shared<std::vector<Durin::uint8>>();
		Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
			[&Renderer, &Scene, &Fixture, Pixels](
				Durin::FRHICommandListImmediate& CommandList) {
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
					Fixture.Name.c_str(), CaptureWidth, CaptureHeight,
					Durin::EPixelFormat::SRGBA8_UNORM)
					.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
						| Durin::ETextureCreateFlags::ShaderResource
						| Durin::ETextureCreateFlags::CPUReadback);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				View.ViewLocation = {Fixture.CameraTranslationX, 0.0, 0.0};
				View.ViewMatrix = glm::translate(Durin::FMatrix(1.0),
					Durin::FVector3(-Fixture.CameraTranslationX, 0.0, 0.0));
				if (Fixture.bPerspective)
				{
					const double YScale = 1.0 / std::tan(
						Durin::Math::DegreesToRadians(60.0) * 0.5);
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
				View.Settings.RenderMode = Fixture.RenderMode;
				View.Settings.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.DirectionalShadowDiagnosticMode =
					Fixture.DiagnosticMode;
				View.Settings.DirectionalShadowFilterQuality =
					Fixture.FilterQuality;
				View.Settings.DirectionalShadowCandidate = Fixture.Candidate;
				// Contact-shadow supplement stays off for the shadow-map-only baseline.
				View.Settings.bEnableContactShadows = false;
				EXPECT_EQ(Renderer.RenderView(
					CommandList, &Scene, View, Target, false, {}),
					Durin::ERenderViewResult::Success);
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					CommandList, Target, 0, 0, *Pixels));
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
		ASSERT_EQ(Pixels->size(),
			static_cast<size_t>(CaptureWidth) * CaptureHeight * 4u);
		if (Fixture.bCastShadows)
		{
			EXPECT_EQ(GLastCounters.ShadowSelectedLights, 1u);
			EXPECT_EQ(GLastCounters.ShadowValidReceiverViews, 1u);
			EXPECT_GT(GLastCounters.ShadowSuccessfulDraws, 0u);
			EXPECT_EQ(GLastCounters.ShadowDiagnosticViews[
				static_cast<size_t>(Fixture.DiagnosticMode)], 1u);
			EXPECT_EQ(GLastCounters.ShadowQualityViews[
				static_cast<size_t>(Fixture.FilterQuality)], 1u);
			const Durin::FDirectionalShadowFilter ExpectedFilter =
				Durin::PrepareDirectionalShadowFilter(Fixture.FilterQuality);
			EXPECT_EQ(GLastCounters.ShadowComparisonOperations,
				ExpectedFilter.ComparisonOperations);
			EXPECT_EQ(GLastCounters.ShadowGuardTexels,
				ExpectedFilter.GuardTexels);
			if (Fixture.Candidate
				== Durin::EDirectionalShadowCandidate::ThreeCascades)
			{
				EXPECT_EQ(GLastCounters.ShadowCandidate, Fixture.Candidate);
				EXPECT_EQ(GLastCounters.ShadowCascadeCount,
					Durin::DirectionalShadowCascadeCount);
				EXPECT_EQ(GLastCounters.ShadowComparisonOperations, 9u);
				EXPECT_EQ(
					GLastCounters.ShadowTransitionComparisonOperations, 18u);
				EXPECT_EQ(GLastCounters.ShadowTargetLogicalBytes,
					Durin::DirectionalShadowLogicalBytes);
				EXPECT_GE(GLastCounters.ShadowTargetBackendBytes,
					GLastCounters.ShadowTargetLogicalBytes);
				EXPECT_LE(GLastCounters.ShadowTargetBackendBytes, 64ull * 1024 * 1024);
				size_t CascadeAttempts = 0;
				for (Durin::uint32 Cascade = 0;
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
			for (const size_t DiagnosticViews
				: GLastCounters.ShadowDiagnosticViews)
				EXPECT_EQ(DiagnosticViews, 0u);
		}
		WritePpm(OutputDirectory / (Fixture.Name + ".ppm"), *Pixels);
		Statistics.push_back(
			CalculateStatistics(Fixture.Name, *Pixels, GLastCounters));
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
		EXPECT_EQ(Statistics[CaptureIndex].Hash,
			Q1FilterTrialExpectedHashes[Index])
			<< Statistics[CaptureIndex].Name;
	}
	for (size_t Index = 0; Index < MediumParityExpectedHashes.size(); ++Index)
	{
		EXPECT_EQ(Statistics[MediumParityStart + Index].Hash,
			MediumParityExpectedHashes[Index])
			<< Statistics[MediumParityStart + Index].Name;
		EXPECT_EQ(Statistics[HighParityStart + Index].Hash,
			HighParityExpectedHashes[Index])
			<< Statistics[HighParityStart + Index].Name;
	}
	for (size_t Index = 0; Index < FilterDiagnosticExpectedHashes.size(); ++Index)
	{
		EXPECT_EQ(Statistics[FilterDiagnosticStart + Index].Hash,
			FilterDiagnosticExpectedHashes[Index])
			<< Statistics[FilterDiagnosticStart + Index].Name;
	}
	EXPECT_NE(Captures[CascadeFixtureStart], Captures[8]);
	const std::array<size_t, 3> MotionChangedPixels{
		CountChangedPixels(Captures[9], Captures[10], 2),
		CountChangedPixels(Captures[10], Captures[11], 2),
		CountChangedPixels(Captures[9], Captures[12], 2)};
	const std::array<size_t, 2> Q1EntryMotionChangedPixels{
		CountChangedPixels(Captures[28], Captures[29], 2),
		CountChangedPixels(Captures[28], Captures[30], 2)};
	EXPECT_LT(MotionChangedPixels[0], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MotionChangedPixels[1], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MotionChangedPixels[2], CaptureWidth * CaptureHeight / 4u);
	EXPECT_LE(Q1EntryMotionChangedPixels[0], 132u);
	EXPECT_LE(Q1EntryMotionChangedPixels[1], 132u);
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
		CalculateShadowOnlyHighFrequencyFraction(Captures[36], Captures[31])};
	const std::array<size_t, 2> MediumMotion{
		CountChangedPixels(Captures[33], Captures[34], 2),
		CountChangedPixels(Captures[33], Captures[35], 2)};
	const std::array<size_t, 2> HighMotion{
		CountChangedPixels(Captures[37], Captures[38], 2),
		CountChangedPixels(Captures[37], Captures[39], 2)};
	const std::array<size_t, 3> MediumQ0Motion{
		CountChangedPixels(Captures[MediumParityStart + 9u],
			Captures[MediumParityStart + 10u], 2),
		CountChangedPixels(Captures[MediumParityStart + 10u],
			Captures[MediumParityStart + 11u], 2),
		CountChangedPixels(Captures[MediumParityStart + 9u],
			Captures[MediumParityStart + 12u], 2)};
	const std::array<size_t, 3> HighQ0Motion{
		CountChangedPixels(Captures[HighParityStart + 9u],
			Captures[HighParityStart + 10u], 2),
		CountChangedPixels(Captures[HighParityStart + 10u],
			Captures[HighParityStart + 11u], 2),
		CountChangedPixels(Captures[HighParityStart + 9u],
			Captures[HighParityStart + 12u], 2)};
	for (const size_t Changed : MediumMotion) EXPECT_LE(Changed, 132u);
	EXPECT_LT(MediumQ0Motion[0], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MediumQ0Motion[1], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MediumQ0Motion[2], CaptureWidth * CaptureHeight / 4u);
	EXPECT_EQ(HighMotion[0], 32u);
	EXPECT_EQ(HighMotion[1], 49u);
	EXPECT_LE(ShadowOnlyHighFrequencyFraction[1],
		ShadowOnlyHighFrequencyFraction[0] * 0.85);
	EXPECT_LE(ShadowOnlyHighFrequencyFraction[2],
		ShadowOnlyHighFrequencyFraction[1] * 0.90);
	const size_t LowTransitionWidth = MaximumTransitionWidth(Captures[23]);
	const size_t MediumTransitionWidth =
		MaximumTransitionWidth(Captures[MediumParityStart + 13u]);
	const size_t HighTransitionWidth =
		MaximumTransitionWidth(Captures[HighParityStart + 13u]);
	EXPECT_LE(MediumTransitionWidth, LowTransitionWidth + 4u);
	EXPECT_LE(HighTransitionWidth, LowTransitionWidth + 8u);
	EXPECT_EQ(Captures[MediumParityStart], Captures[0]);
	EXPECT_EQ(Captures[MediumParityStart + 6u],
		Captures[MediumParityStart + 7u]);
	EXPECT_EQ(Captures[HighParityStart + 6u],
		Captures[HighParityStart + 7u]);
	EXPECT_EQ(Captures[MediumParityStart + 8u], Captures[8]);
	EXPECT_EQ(Captures[HighParityStart + 8u], Captures[8]);
	EXPECT_NE(Captures[MediumParityStart + 3u],
		Captures[MediumParityStart + 4u]);
	EXPECT_NE(Captures[HighParityStart + 3u],
		Captures[HighParityStart + 4u]);
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
	WriteMetrics(OutputDirectory / "baseline-metrics.json", Statistics,
		MotionChangedPixels, Q1EntryMotionChangedPixels,
		ShadowOnlyHighFrequencyFraction, MediumMotion, HighMotion);
	std::cout << "Directional shadow Q0 baseline artifacts: "
		<< OutputDirectory.string() << '\n'
		<< "Directional shadow Q1 Low entry motion pixels: "
		<< Q1EntryMotionChangedPixels[0] << ", "
		<< Q1EntryMotionChangedPixels[1] << '\n';

	Durin::SetViewRenderCounterSink(nullptr);
	auto RendererShutdownContext = Durin::FModuleTestContextFactory::CreateShutdownContext(RendererContext);
	Renderer.ShutdownModule(RendererShutdownContext);
	Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
		[&](Durin::FRHICommandListImmediate&) { Quad->ReleaseResources(); });
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}

TEST(FDirectionalShadowBaselineVulkanTests,
	ContactShadowRunsAndDarkensNearFieldBounded)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	Durin::FRendererModule Renderer;
	Renderer.StartupModule();
	Durin::SetViewRenderCounterSink(CaptureCounters);

	auto Quad = MakeQuadRenderData();
	Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(Quad->InitResources(CommandList));
		});
	Durin::FlushRenderingCommands();
	auto Opaque = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque, {0.72, 0.72, 0.72});

	// Ground quad plus a floating occluder: the exact contact-detachment
	// scenario where necessary bias leaves a detached shadow the screen-space
	// supplement should refill.
	Durin::FScene Scene;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(1),
		std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1),
		MakeTransform({.Translation = {0.0, 0.0, -0.5},
			.Scale = {0.82, 0.82, 1.0}}));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2),
		std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1),
		MakeTransform({.Translation = {-0.18, 0.08, -0.4},
			.Scale = {0.22, 0.18, 1.0}}));
	// A vertical wall whose visible face is back-facing relative to the light:
	// the screen-space supplement must NOT self-occlude it. The camera looks
	// toward -z (projection maps smaller z to nearer), so the floor at negative
	// z presents its +z face, which faces the light.
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(3),
		std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1),
		MakeTransform({.Translation = {-0.35, 0.0, -0.45},
			.Scale = {0.25, 0.25, 1.0}, .RotationYDegrees = 90.0}));
	Durin::FDirectionalLightSceneData Directional;
	Directional.Direction = {0.35, 0.2, -1.0};
	Directional.Color = {1.0f, 1.0f, 1.0f};
	Directional.Intensity = 3.0f;
	Directional.bCastShadows = true;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(100),
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Durin::FlushRenderingCommands();

	auto RenderCapture = [&](bool bEnableContactShadows,
		bool bShowContactShadowDebug,
		std::vector<Durin::uint8>& OutPixels) -> Durin::FViewRenderCounters
	{
		auto Pixels = std::make_shared<std::vector<Durin::uint8>>();
		Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
			[&Renderer, &Scene, bEnableContactShadows,
				bShowContactShadowDebug, Pixels](
				Durin::FRHICommandListImmediate& CommandList) {
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
					"ContactShadowCapture", CaptureWidth, CaptureHeight,
					Durin::EPixelFormat::SRGBA8_UNORM)
					.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
						| Durin::ETextureCreateFlags::ShaderResource
						| Durin::ETextureCreateFlags::CPUReadback);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				View.ViewMatrix = Durin::FMatrix(1.0);
				View.ProjectionMatrix = Durin::FMatrix(1.0);
				View.ProjectionMatrix[2][2] = -1.0;
				View.ProjectionMatrix[3][2] = 0.0;
				View.ViewProjectionMatrix =
					View.ProjectionMatrix * View.ViewMatrix;
				View.ViewportWidth = CaptureWidth;
				View.ViewportHeight = CaptureHeight;
				View.Settings.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.DirectionalShadowCandidate =
					Durin::EDirectionalShadowCandidate::SingleMap;
				View.Settings.DirectionalShadowFilterQuality =
					Durin::EDirectionalShadowFilterQuality::Low;
				View.Settings.bEnableContactShadows = bEnableContactShadows;
				View.Settings.bShowContactShadowDebug =
					bShowContactShadowDebug;
				EXPECT_EQ(Renderer.RenderView(
					CommandList, &Scene, View, Target, false, {}),
					Durin::ERenderViewResult::Success);
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					CommandList, Target, 0, 0, *Pixels));
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
		EXPECT_EQ(Pixels->size(),
			static_cast<size_t>(CaptureWidth) * CaptureHeight * 4u);
		OutPixels = std::move(*Pixels);
		return GLastCounters;
	};

	std::vector<Durin::uint8> PixelsOff;
	const Durin::FViewRenderCounters CountersOff =
		RenderCapture(false, false, PixelsOff);
	std::vector<Durin::uint8> PixelsOn;
	const Durin::FViewRenderCounters CountersOn =
		RenderCapture(true, false, PixelsOn);

	// The pass must run exactly once when enabled, zero when disabled, and
	// never report a resource/input failure.
	EXPECT_EQ(CountersOff.ContactShadowEnabledViews, 0u);
	EXPECT_EQ(CountersOn.ContactShadowEnabledViews, 1u);
	EXPECT_EQ(CountersOn.ContactShadowPassFailures, 0u);

	// The enabled image must differ from the disabled one and the difference
	// must stay bounded (near-field contact, not a whole-frame darkening).
	EXPECT_NE(PixelsOn, PixelsOff);
	const size_t ChangedPixels = CountChangedPixels(PixelsOn, PixelsOff, 2);
	EXPECT_GT(ChangedPixels, 0u);
	EXPECT_LT(ChangedPixels, CaptureWidth * CaptureHeight / 2u);

	// The editor's contact-contribution diagnostic is a bounded red mask, not
	// another lighting mode. Keep it covered by the same near-field fixture.
	std::vector<Durin::uint8> DebugPixels;
	const Durin::FViewRenderCounters DebugCounters =
		RenderCapture(true, true, DebugPixels);
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

	const FCaptureStatistics StatsOff =
		CalculateStatistics("contact_off", PixelsOff, CountersOff);
	const FCaptureStatistics StatsOn =
		CalculateStatistics("contact_on", PixelsOn, CountersOn);
	const std::filesystem::path OutputDirectory =
		Durin::Testing::CreateTestFixtureDirectory(
			"DirectionalContactShadow");
	WritePpm(OutputDirectory / "contact_shadow_off.ppm", PixelsOff);
	WritePpm(OutputDirectory / "contact_shadow_on.ppm", PixelsOn);
	std::cout << "Contact shadow changed pixels: " << ChangedPixels
		<< " (off dark/mid/bright " << StatsOff.DarkPixels << "/"
		<< StatsOff.MidPixels << "/" << StatsOff.BrightPixels
		<< ", on " << StatsOn.DarkPixels << "/" << StatsOn.MidPixels << "/"
		<< StatsOn.BrightPixels << ")\n";

	// Contact shadows may only attenuate the selected directional direct term.
	// Preserve the same depth and occluder configuration while rendering it
	// unlit: the pass still runs, but it has no contribution to remove.
	auto Unlit = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque,
		{0.35, 0.22, 0.12},
		Durin::EMaterialShadingModel::Unlit);
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(1),
		std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Unlit}, 1),
		MakeTransform({.Translation = {0.0, 0.0, -0.5},
			.Scale = {0.82, 0.82, 1.0}}));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2),
		std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Unlit}, 1),
		MakeTransform({.Translation = {-0.18, 0.08, -0.4},
			.Scale = {0.22, 0.18, 1.0}}));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(3),
		std::make_unique<Durin::FStaticMeshSceneProxy>(Quad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Unlit}, 1),
		MakeTransform({.Translation = {-0.35, 0.0, -0.45},
			.Scale = {0.25, 0.25, 1.0}, .RotationYDegrees = 90.0}));
	Durin::FlushRenderingCommands();
	std::vector<Durin::uint8> UnlitOff;
	std::vector<Durin::uint8> UnlitOn;
	RenderCapture(false, false, UnlitOff);
	const Durin::FViewRenderCounters UnlitCounters =
		RenderCapture(true, false, UnlitOn);
	EXPECT_EQ(UnlitCounters.ContactShadowEnabledViews, 1u);
	EXPECT_EQ(UnlitCounters.ContactShadowPassFailures, 0u);
	EXPECT_EQ(UnlitOn, UnlitOff);

	Durin::SetViewRenderCounterSink(nullptr);
	Renderer.ShutdownModule();
	Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
		[&](Durin::FRHICommandListImmediate&) { Quad->ReleaseResources(); });
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
