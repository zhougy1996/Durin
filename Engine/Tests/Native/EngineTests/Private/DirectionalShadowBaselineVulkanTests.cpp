#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialRenderProxy.h"
#include "Modules/ModuleManager.h"
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
#include <filesystem>
#include <fstream>
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
		const Durin::FVector3& BaseColor) -> Durin::FMaterialRenderProxyRef
	{
		auto Proxy = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
		Durin::FMaterialRenderProxyPublication Publication;
		Publication.LocalVersion = 1;
		Publication.LocalLayer.StaticProperties = Durin::FMaterialStaticProperties{
			.BlendMode = BlendMode,
			.ShadingModel = Durin::EMaterialShadingModel::Lit,
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
		const std::array<size_t, 3>& MotionChangedPixels) -> void
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
			<< MotionChangedPixels[2] << "]\n}\n";
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
	auto Masked = MakeMaterial(
		Durin::EMaterialBlendMode::Masked, {0.72, 0.72, 0.72});

	const std::vector<FFixture> Fixtures{
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
		 .RenderMode = Durin::ERenderMode::Unlit}};
	constexpr std::array<std::string_view, 13> ExpectedHashes{
		"d2cc1b606edb268b4bcc68a18d624b4d",
		"4d0ac6bb59200618b95c72e76bae8ea1",
		"3148cb2469690824c8b1fd4c6d28c802",
		"56f6263d3bfcfd7916549eae4f9e5e12",
		"ab5d52758cae1d9a3ae54c082b623fb7",
		"0d107e2f124f290e5baa6025a821c543",
		"303a0d4d212b4f40760efcbcc593100e",
		"303a0d4d212b4f40760efcbcc593100e",
		"1fa526cb259e748e81027df9fe529c1d",
		"56f6263d3bfcfd7916549eae4f9e5e12",
		"de843aa6186a499fb1b8eecbd316ad7e",
		"a7cb654c08677766bdfba5cfd63f0103",
		"ead3a108ecf314d076bad9828f92de33"};

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
				View.ProjectionMatrix = Durin::FMatrix(1.0);
				View.ProjectionMatrix[2][2] = -1.0;
				View.ProjectionMatrix[3][2] = 1.0;
				View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
				View.ViewportWidth = CaptureWidth;
				View.ViewportHeight = CaptureHeight;
				View.Settings.RenderMode = Fixture.RenderMode;
				View.Settings.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.DirectionalShadowDiagnosticMode =
					Fixture.DiagnosticMode;
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
		Scene.Release();
		Durin::FlushRenderingCommands();
	}

	ASSERT_EQ(Captures.size(), Fixtures.size());
	ASSERT_GE(Statistics.size(), ExpectedHashes.size());
	for (size_t Index = 0; Index < ExpectedHashes.size(); ++Index)
	{
		EXPECT_EQ(Statistics[Index].Hash, ExpectedHashes[Index])
			<< Statistics[Index].Name;
	}
	const std::array<size_t, 3> MotionChangedPixels{
		CountChangedPixels(Captures[9], Captures[10], 2),
		CountChangedPixels(Captures[10], Captures[11], 2),
		CountChangedPixels(Captures[9], Captures[12], 2)};
	EXPECT_LT(MotionChangedPixels[0], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MotionChangedPixels[1], CaptureWidth * CaptureHeight / 8u);
	EXPECT_LT(MotionChangedPixels[2], CaptureWidth * CaptureHeight / 4u);
	EXPECT_NE(Captures[3], Captures[4]);
	EXPECT_EQ(Captures[6], Captures[7]);
	EXPECT_NE(Captures[2], Captures[8]);
	EXPECT_NE(Captures[3], Captures[8]);
	for (size_t Index = 13; Index < Captures.size(); ++Index)
	{
		EXPECT_NE(Captures[Index], Captures[3]) << Fixtures[Index].Name;
	}
	EXPECT_NE(Captures[14], Captures[15]);
	EXPECT_NE(Captures[15], Captures[16]);
	EXPECT_EQ(Captures[8], Captures[20]);
	EXPECT_EQ(Captures[21], Captures[22]);
	WriteMetrics(OutputDirectory / "baseline-metrics.json", Statistics,
		MotionChangedPixels);
	std::cout << "Directional shadow Q0 baseline artifacts: "
		<< OutputDirectory.string() << '\n';

	Durin::SetViewRenderCounterSink(nullptr);
	Renderer.ShutdownModule();
	Durin::EnqueueRenderCommand<FShadowBaselineCommand>(
		[&](Durin::FRHICommandListImmediate&) { Quad->ReleaseResources(); });
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
