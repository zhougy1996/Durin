#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/SkeletalMeshSceneProxy.h"
#include "Engine/SplineMeshSceneProxy.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "GBufferContract.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/DirectionalShadowRenderer.h"
#include "Scene.h"
#include "SceneView.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "Spline/SplineMeshDeformer.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

namespace Durin
{
	class FTestSkeletalMeshVertexShader : public FShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FTestSkeletalMeshVertexShader)
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_SHADER_PARAMETER_STORAGE_BUFFER(SkinPalette);
		DURIN_END_SHADER_PARAMETERS();

		DURIN_DECLARE_SHADER(
			FTestSkeletalMeshVertexShader,
			FShader,
			"/Engine/StaticMeshBasePass",
			EShaderFrequency::Vertex,
			"VertexMain"
		);
	};
} // namespace Durin

namespace
{
	Durin::FViewRenderCounters GLastCounters;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GSceneColorTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GShadowDepthTimingQueries = nullptr;
	std::array<std::vector<Durin::uint8>*, 4> GGBufferPixels{};
	auto CaptureCounters(const Durin::FViewRenderCounters& Counters) -> void
	{
		GLastCounters = Counters;
	}
	auto CaptureShadowDepthTiming(
		const Durin::FGPUTimingQueryRHIRef& Query
	) -> void
	{
		if (GShadowDepthTimingQueries != nullptr)
			GShadowDepthTimingQueries->push_back(Query);
	}
	auto CaptureSceneColorTiming(
		const Durin::FGPUTimingQueryRHIRef& Query
	) -> void
	{
		if (GSceneColorTimingQueries != nullptr)
			GSceneColorTimingQueries->push_back(Query);
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
		const std::array Sources{Material, Normals, Surface, Emissive};
		const std::array Names{"SkeletalGBufferMaterial", "SkeletalGBufferNormals", "SkeletalGBufferSurface", "SkeletalGBufferEmissive"};
		for (size_t Index = 0; Index < Sources.size(); ++Index)
		{
			if (GGBufferPixels[Index] == nullptr) continue;
			Durin::FRHITexture* Source = Sources[Index];
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  Names[Index], Source->GetSizeX(), Source->GetSizeY(),
								  Source->GetFormat()
			)
								  .SetFlags(Durin::ETextureCreateFlags::DestinationCopy | Durin::ETextureCreateFlags::CPUReadback | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Readback =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Readback, nullptr);
			const Durin::FRHITextureSubresourceRange Whole{
				Durin::ERHITextureAspect::Color, 0, 1, 0, 1
			};
			CommandList.TransitionTextures(std::array{
				Durin::FRHITextureTransition{Source, Whole, Durin::ERHIAccess::GraphicsShaderRead, Durin::ERHIAccess::TransferRead},
				Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::Discard, Durin::ERHIAccess::TransferWrite}
			});
			CommandList.CopyTexture(Source, Readback, std::array{Durin::FRHITextureCopyRegion{.Extent = {Source->GetSizeX(), Source->GetSizeY(), 1}}});
			CommandList.TransitionTextures(std::array{
				Durin::FRHITextureTransition{Source, Whole, Durin::ERHIAccess::TransferRead, Durin::ERHIAccess::GraphicsShaderRead},
				Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead}
			});
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Readback, 0, 0, *GGBufferPixels[Index]
			));
		}
	}

	auto ValidateCapturedGBuffer(
		const std::array<std::vector<Durin::uint8>, 4>& Pixels
	) -> void
	{
		ASSERT_FALSE(Pixels[0].empty());
		for (const auto& Attachment : Pixels)
			ASSERT_EQ(Attachment.size(), Pixels[0].size());
		size_t ValidPixels = 0;
		for (size_t Offset = 0; Offset < Pixels[2].size(); Offset += 4)
		{
			if (Pixels[2][Offset + 3] == 0u) continue;
			++ValidPixels;
			EXPECT_EQ(Pixels[2][Offset + 3], Durin::GBufferContract::StandardLitFlag);
			auto DecodeNormal = [&Pixels, Offset](size_t PairOffset) {
				return Durin::GBufferContract::DecodeOctahedralNormal({static_cast<float>(Pixels[1][Offset + PairOffset]) / 255.0f, static_cast<float>(Pixels[1][Offset + PairOffset + 1]) / 255.0f});
			};
			EXPECT_NEAR(Durin::Math::Length(DecodeNormal(0)), 1.0, 1.0e-5);
			EXPECT_NEAR(Durin::Math::Length(DecodeNormal(2)), 1.0, 1.0e-5);
			EXPECT_GE(static_cast<Durin::uint32>(Pixels[0][Offset]) + Pixels[0][Offset + 1] + Pixels[0][Offset + 2], 254u);
		}
		EXPECT_GT(ValidPixels, 0u);
	}

	auto MakeRenderData(bool bComplete = true)
		-> std::unique_ptr<Durin::FSkeletalMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FSkeletalMeshRenderData>();
		const std::vector<Durin::FVector3f> Positions{
			{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}
		};
		Data->VertexBuffers.Geometry.PositionVertexBuffer.Init(Positions);
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(3, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(3, {1.0f, 0.0f, 0.0f, 1.0f})
		);
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels> UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), 3, 1
		);
		Data->VertexBuffers.Geometry.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(3, Durin::FVector4f(1.0f)), 3
		);
		if (bComplete)
		{
			Durin::FSkeletalMeshVertexInfluences First;
			First.BoneIndices[0] = 0;
			First.Weights[0] = 1.0f;
			First.Count = 1;
			Durin::FSkeletalMeshVertexInfluences Mixed;
			Mixed.BoneIndices[0] = 0;
			Mixed.BoneIndices[1] = 1;
			Mixed.Weights[0] = 0.5f;
			Mixed.Weights[1] = 0.5f;
			Mixed.Count = 2;
			Durin::FSkeletalMeshVertexInfluences Second;
			Second.BoneIndices[0] = 1;
			Second.Weights[0] = 1.0f;
			Second.Count = 1;
			Data->VertexBuffers.InfluenceVertexBuffer.Init(
				{First, Mixed, Second}
			);
		}
		Data->IndexBuffer.Init({0, 1, 2, 0, 1, 2, 0, 1, 2});
		for (Durin::uint32 SectionIndex = 0; SectionIndex < 3; ++SectionIndex)
			Data->Sections.push_back({.Name = Durin::FName(std::format("Section{}", SectionIndex)), .FirstIndex = SectionIndex * 3, .IndexCount = 3, .MinVertexIndex = 0, .MaxVertexIndex = 2, .MaterialSlotIndex = SectionIndex, .LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})});
		Data->MaterialSlots = {
			Durin::FName("Opaque"), Durin::FName("Masked"),
			Durin::FName("Translucent")
		};
		Data->PaletteBoneIndices = {0, 1};
		Data->InverseBindMatrices = {
			Durin::FMatrix4f(1.0f), Durin::FMatrix4f(1.0f)
		};
		Data->InfluenceBounds = {
			Durin::FBox({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}),
			Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})
		};
		Data->LocalBounds = Data->Sections[0].LocalBounds;
		return Data;
	}

	auto MakePerspectiveProjection(
		double VerticalFieldOfViewDegrees,
		double AspectRatio,
		double NearClip,
		double FarClip
	) -> Durin::FMatrix
	{
		const double YScale = 1.0 / std::tan(
			Durin::Math::DegreesToRadians(VerticalFieldOfViewDegrees) * 0.5
		);
		Durin::FMatrix Projection(0.0);
		Projection[1][0] = YScale / AspectRatio;
		Projection[2][1] = -YScale;
		Projection[0][2] = FarClip / (FarClip - NearClip);
		Projection[3][2] = -NearClip * FarClip / (FarClip - NearClip);
		Projection[0][3] = 1.0;
		return Projection;
	}

	auto MakeSplineSourceRenderData() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FStaticMeshRenderData>();
		Data->MaterialSlots = {{"Opaque", 0}, {"Masked", 1}, {"Translucent", 2}};
		Data->LODResources.resize(1);
		auto& LOD = Data->LODResources[0];
		LOD.VertexBuffers.PositionVertexBuffer.Init({{-0.8f, -0.25f, 0.0f}, {0.8f, -0.25f, 0.0f}, {0.0f, 0.25f, 0.0f}});
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(3, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(3, {1.0f, 0.0f, 0.0f, 1.0f})
		);
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels> UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(UVs), 3, 1);
		LOD.VertexBuffers.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(3, Durin::FVector4f(1.0f)), 3
		);
		LOD.IndexBuffer.Init({0, 1, 2, 0, 1, 2, 0, 1, 2});
		for (Durin::uint32 SectionIndex = 0; SectionIndex < 3; ++SectionIndex)
			LOD.Sections.push_back({.Name = std::format("Spline{}", SectionIndex), .FirstIndex = SectionIndex * 3, .IndexCount = 3, .MinVertexIndex = 0, .MaxVertexIndex = 2, .MaterialSlotIndex = SectionIndex, .LocalBounds = Durin::FBox({-0.8, -0.25, 0.0}, {0.8, 0.25, 0.0})});
		LOD.LocalBounds = LOD.Sections[0].LocalBounds;
		Data->LODVertexFactories.resize(1);
		Data->RecalculateBounds();
		return Data;
	}

	auto MakeSplineRoadRenderData() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FStaticMeshRenderData>();
		Data->MaterialSlots = {{"Opaque", 0}};
		auto& LOD = Data->LODResources.emplace_back();
		std::vector<Durin::FVector3f> Positions;
		std::vector<Durin::uint32> Indices;
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels> UVs;
		Positions.reserve(256);
		UVs[0].reserve(256);
		Indices.reserve(254 * 3);
		for (Durin::uint32 Slice = 0; Slice < 128; ++Slice)
		{
			const float X = static_cast<float>(Slice) / 127.0f;
			Positions.push_back({X, -0.5f, 0.0f});
			Positions.push_back({X, 0.5f, 0.0f});
			UVs[0].push_back({X, 0.0f});
			UVs[0].push_back({X, 1.0f});
			if (Slice == 0) continue;
			const Durin::uint32 Base = (Slice - 1) * 2;
			Indices.insert(Indices.end(), {Base, Base + 1, Base + 2, Base + 1, Base + 3, Base + 2});
		}
		LOD.VertexBuffers.PositionVertexBuffer.Init(std::move(Positions));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(256, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(256, {1.0f, 0.0f, 0.0f, 1.0f})
		);
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(UVs), 256, 1);
		LOD.VertexBuffers.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(256, Durin::FVector4f(1.0f)), 256
		);
		LOD.IndexBuffer.Init(std::move(Indices));
		LOD.Sections.push_back({.Name = "Road", .FirstIndex = 0, .IndexCount = 254 * 3, .MinVertexIndex = 0, .MaxVertexIndex = 255, .MaterialSlotIndex = 0, .LocalBounds = Durin::FBox({0.0, -0.5, 0.0}, {1.0, 0.5, 0.0})});
		LOD.LocalBounds = LOD.Sections[0].LocalBounds;
		Data->LODVertexFactories.resize(1);
		Data->RecalculateBounds();
		return Data;
	}

	struct FSkeletalResourceLifecycleCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "SkeletalResourceLifecycle";
		}
	};
} // namespace

TEST(FSkeletalMeshRenderResourcesVulkanTests, InitializesRejectsRetriesAndReleasesExactly)
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
	const Durin::FRHICapabilities* Capabilities =
		Durin::GDynamicRHI->RHIGetCapabilities();
	ASSERT_NE(Capabilities, nullptr);
	EXPECT_GT(Capabilities->MinStorageBufferOffsetAlignment, 0u);
	EXPECT_GE(Capabilities->MaxStorageBufferRange, sizeof(Durin::FMatrix4f));
	Durin::InitRenderingThread();
	Durin::FRendererModule Renderer;
	Durin::FModuleTestHarness RendererLifecycle("SkeletalMeshRendererTest");
	RendererLifecycle.Start(Renderer);
	Durin::SetViewRenderCounterSink(CaptureCounters);

	auto Complete = MakeRenderData();
	auto SplineSource = MakeSplineSourceRenderData();
	auto Malformed = MakeRenderData(false);
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			Durin::FShaderCompileOptions CompileOptions;
			CompileOptions.Macros.emplace_back("DURIN_SKELETAL_MESH", "1");
			CompileOptions.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", "0");
			CompileOptions.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", "0");
			CompileOptions.Macros.emplace_back(
				"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", "0"
			);
			Durin::FShaderType& VertexShaderType =
				Durin::FTestSkeletalMeshVertexShader::StaticType();
			const std::array<const Durin::FShaderType*, 1> ShaderTypes{
				&VertexShaderType
			};
			Durin::FShaderMapBase ShaderMap;
			std::string ShaderError;
			ASSERT_TRUE(ShaderMap.InitializeFromShaderTypes(
				ShaderTypes, CompileOptions, ShaderError
			)) << ShaderError;
			const auto* VertexShader = static_cast<Durin::FTestSkeletalMeshVertexShader*>(
				ShaderMap.GetShader(&VertexShaderType)
			);
			ASSERT_NE(VertexShader, nullptr);
			const auto& ParameterBindings = VertexShader->GetParameterBindings();
			ASSERT_EQ(ParameterBindings.size(), 2u);
			EXPECT_STREQ(ParameterBindings[0].Name, "Transform");
			EXPECT_STREQ(ParameterBindings[1].Name, "SkinPalette");
			EXPECT_EQ(
				ParameterBindings[1].Type,
				Durin::ERHIBindingType::StorageBuffer
			);

			const std::array<Durin::FMatrix4f, 2> Palette{
				Durin::FMatrix4f(1.0f), Durin::FMatrix4f(2.0f)
			};
			const auto FirstRange = CommandList.AllocateDynamicStorageBuffer(
				Palette.data(), sizeof(Palette)
			);
			const auto SecondRange = CommandList.AllocateDynamicStorageBuffer(
				Palette.data(), sizeof(Palette)
			);
			ASSERT_NE(FirstRange.Buffer, nullptr);
			ASSERT_NE(SecondRange.Buffer, nullptr);
			EXPECT_EQ(FirstRange.Size, sizeof(Palette));
			EXPECT_EQ(SecondRange.Size, sizeof(Palette));
			EXPECT_EQ(
				FirstRange.Offset % Capabilities->MinStorageBufferOffsetAlignment,
				0u
			);
			EXPECT_EQ(
				SecondRange.Offset % Capabilities->MinStorageBufferOffsetAlignment,
				0u
			);
			EXPECT_TRUE(
				FirstRange.Buffer != SecondRange.Buffer
				|| FirstRange.Offset + FirstRange.Size <= SecondRange.Offset
				|| SecondRange.Offset + SecondRange.Size <= FirstRange.Offset
			);
			const std::array<Durin::FRHIBufferTransition, 2> PaletteTransitions{
				Durin::FRHIBufferTransition{
					.Buffer = FirstRange.Buffer,
					.Offset = FirstRange.Offset,
					.Size = FirstRange.Size,
					.ExpectedBefore = Durin::ERHIAccess::HostWrite,
					.RequiredAfter = Durin::ERHIAccess::GraphicsShaderRead
				},
				Durin::FRHIBufferTransition{
					.Buffer = SecondRange.Buffer,
					.Offset = SecondRange.Offset,
					.Size = SecondRange.Size,
					.ExpectedBefore = Durin::ERHIAccess::HostWrite,
					.RequiredAfter = Durin::ERHIAccess::GraphicsShaderRead
				}
			};
			CommandList.TransitionBuffers(PaletteTransitions);

			if (Capabilities->MaxStorageBufferRange < UINT32_MAX)
			{
				const std::uint8_t Sentinel = 0;
				const auto RejectedRange = CommandList.AllocateDynamicStorageBuffer(
					&Sentinel, Capabilities->MaxStorageBufferRange + 1u
				);
				EXPECT_EQ(RejectedRange.Buffer, nullptr);
				EXPECT_EQ(RejectedRange.Offset, 0u);
				EXPECT_EQ(RejectedRange.Size, 0u);
			}

			EXPECT_FALSE(Malformed->InitResources(CommandList));
			EXPECT_EQ(Malformed->GetNumInitializedResources(), 0u);
			ASSERT_TRUE(Complete->InitResources(CommandList));
			EXPECT_TRUE(Complete->IsReadyForRendering());
			EXPECT_EQ(Complete->GetNumInitializedResources(), 7u);
			Complete->ReleaseResources();
			EXPECT_EQ(Complete->GetNumInitializedResources(), 0u);
			ASSERT_TRUE(Complete->InitResources(CommandList));
			EXPECT_TRUE(Complete->IsReadyForRendering());
			EXPECT_EQ(Complete->GetNumInitializedResources(), 7u);
			ASSERT_TRUE(SplineSource->InitResources(CommandList));
		}
	);
	Durin::FlushRenderingCommands();

	auto MakeMaterial = [](Durin::EMaterialBlendMode BlendMode,
						   Durin::FVector3 Color,
						   Durin::EMaterialShadingModel ShadingModel =
							   Durin::EMaterialShadingModel::Lit) {
		auto Material = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
		Durin::FMaterialRenderProxyPublication Publication;
		Publication.LocalVersion = 1;
		Publication.LocalLayer.StaticProperties =
			Durin::FMaterialStaticProperties{
				.BlendMode = BlendMode,
				.ShadingModel = ShadingModel,
				.bTwoSided = true
			};
		Publication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::BaseColorId, .Type = Durin::EMaterialParameterType::Vector, .VectorValue = Color});
		EXPECT_TRUE(Material->QueuePublication_GameThread(std::move(Publication)));
		return Material;
	};
	auto Opaque = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque, {1.0, 0.0, 0.0}
	);
	auto Masked = MakeMaterial(
		Durin::EMaterialBlendMode::Masked, {0.0, 1.0, 0.0}
	);
	auto Translucent = MakeMaterial(
		Durin::EMaterialBlendMode::Translucent, {0.0, 0.0, 1.0}
	);
	auto Unlit = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque, {1.0, 1.0, 0.0},
		Durin::EMaterialShadingModel::Unlit
	);
	auto Pose = std::make_shared<Durin::FSkeletalPosePalette>();
	Pose->Revision = 1;
	Pose->SkeletonCompatibilityIdentity = "VulkanSkeletalMesh";
	Pose->Matrices = {Durin::FMatrix4f(1.0f), Durin::FMatrix4f(1.0f)};
	Pose->LocalBounds = Complete->LocalBounds;
	Durin::FScene Scene;
	Scene.AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(1),
		std::make_unique<Durin::FSkeletalMeshSceneProxy>(
			Complete.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent},
			1, Pose
		),
		Durin::FMatrix(1.0)
	);
	Durin::FlushRenderingCommands();

	auto Readback = std::make_shared<std::vector<Durin::uint8>>();
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&Renderer, &Scene, Readback](Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const Durin::FRHITextureCreateDesc Desc =
				Durin::FRHITextureCreateDesc::Create2D(
					"SkeletalMeshValidationColor", 33, 33,
					Durin::EPixelFormat::SRGBA8_UNORM
				)
					.SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource | Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 33;
			View.ViewportHeight = 33;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			(void)Renderer.RenderView(
				CommandList, &Scene, View, Target, false, {}
			);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Target, 0, 0, *Readback
			));
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		}
	);
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Readback->size(), 33u * 33u * 4u);
	EXPECT_EQ(GLastCounters.VisibleSkeletalMeshCandidates, 1u);
	EXPECT_EQ(GLastCounters.PreparedSkeletalMeshPrimitives, 1u);
	EXPECT_EQ(GLastCounters.PreparedSkeletalMeshSections, 3u);
	EXPECT_EQ(GLastCounters.OpaqueSkeletalMeshSections, 1u);
	EXPECT_EQ(GLastCounters.MaskedSkeletalMeshSections, 1u);
	EXPECT_EQ(GLastCounters.TranslucentSkeletalMeshSections, 1u);
	EXPECT_EQ(GLastCounters.PreparedSkeletalMeshTriangles, 3u);
	EXPECT_EQ(GLastCounters.OpaqueSkeletalMeshStateGroups, 1u);
	EXPECT_EQ(GLastCounters.MaskedSkeletalMeshStateGroups, 1u);
	EXPECT_EQ(GLastCounters.CombinedTranslucentGeometryDraws, 1u);
	EXPECT_EQ(GLastCounters.RequestedSkeletalPaletteUploads, 1u);
	EXPECT_EQ(GLastCounters.UploadedSkeletalPalettes, 1u);
	EXPECT_EQ(GLastCounters.UploadedSkeletalPaletteMatrices, 2u);
	EXPECT_EQ(GLastCounters.UploadedSkeletalPaletteBytes, 2u * sizeof(Durin::FMatrix4f));
	EXPECT_EQ(GLastCounters.SkeletalMeshResourceAttemptedDraws, 3u);
	EXPECT_EQ(GLastCounters.SkeletalMeshResourceSuccessfulDraws, 3u);
	EXPECT_EQ(GLastCounters.SkeletalMeshSuccessfulDraws, 3u);
	EXPECT_TRUE(std::ranges::any_of(*Readback, [](Durin::uint8 Value) { return Value != 0; }));
	size_t RedPixels = 0;
	for (size_t Offset = 0; Offset + 3 < Readback->size(); Offset += 4)
		RedPixels += (*Readback)[Offset] > (*Readback)[Offset + 1] + 20
							 && (*Readback)[Offset] > (*Readback)[Offset + 2] + 20 ?
						 1u :
						 0u;
	EXPECT_GT(RedPixels, 0u);
	auto RenderLitReadback = [&](
		std::string Name,
		bool bEnableGBufferQualification = false,
		Durin::EDirectionalShadowCandidate ShadowCandidate =
			Durin::EDirectionalShadowCandidate::SingleMap,
		Durin::EViewVisibilityMode VisibilityMode =
			Durin::EViewVisibilityMode::FrustumCullingDisabled,
		Durin::FMatrix ViewProjection = Durin::FMatrix(1.0)
	) {
		auto Result = std::make_shared<std::vector<Durin::uint8>>();
		Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
			[&Renderer, &Scene, Result, Name = std::move(Name),
			 bEnableGBufferQualification, ShadowCandidate, VisibilityMode,
			 ViewProjection](
				Durin::FRHICommandListImmediate& CommandList
			) {
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
									  Name.c_str(), 33, 33, Durin::EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource | Durin::ETextureCreateFlags::CPUReadback);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				View.ProjectionMatrix = ViewProjection;
				View.ViewProjectionMatrix = ViewProjection;
				View.ViewportWidth = 33;
				View.ViewportHeight = 33;
				View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
				View.Settings.DirectionalShadow.Candidate =
					ShadowCandidate;
				View.Settings.Mode.VisibilityMode =
					VisibilityMode;
				Durin::FSceneViewRenderOptions RenderOptions;
				RenderOptions.bEnableGBufferQualification =
					bEnableGBufferQualification;
				RenderOptions.bEnableDeferredDirectionalQualification =
					bEnableGBufferQualification;
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, RenderOptions), Durin::ERenderViewResult::Success);
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					CommandList, Target, 0, 0, *Result
				));
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		return Result;
	};
	const auto ZeroLightReadback = RenderLitReadback("ZeroLightSkeletalColor");
	std::array<std::vector<Durin::uint8>, 4> SkeletalGBufferPixels;
	for (size_t Index = 0; Index < GGBufferPixels.size(); ++Index)
		GGBufferPixels[Index] = &SkeletalGBufferPixels[Index];
	Durin::SetGBufferCaptureSink(CaptureGBuffer);
	const auto GBufferReadback = RenderLitReadback(
		"GBufferSkeletalColor", true
	);
	Durin::SetGBufferCaptureSink(nullptr);
	GGBufferPixels.fill(nullptr);
	EXPECT_EQ(*GBufferReadback, *ZeroLightReadback);
	ValidateCapturedGBuffer(SkeletalGBufferPixels);
	EXPECT_EQ(GLastCounters.GBufferEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.GBufferAttemptedDraws, 2u);
	EXPECT_EQ(GLastCounters.GBufferSuccessfulDraws, 2u);
	EXPECT_EQ(GLastCounters.GBufferRejectedDraws, 0u);
	EXPECT_EQ(GLastCounters.GBufferSkippedDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferSkeletalMeshAttemptedDraws, 2u);
	EXPECT_EQ(GLastCounters.GBufferSkeletalMeshSuccessfulDraws, 2u);
	EXPECT_EQ(GLastCounters.GBufferSkeletalMeshRejectedDraws, 0u);
	EXPECT_EQ(GLastCounters.GBufferSkeletalMeshSkippedDraws, 1u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalUnavailableViews, 0u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalPassFailures, 0u);
	Durin::FDirectionalLightSceneData Directional;
	Directional.Direction = {0.0, 0.0, -1.0};
	Directional.Color = {1.0f, 0.1f, 0.1f};
	Directional.Intensity = 2.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(10), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Durin::FPointLightSceneData Point;
	Point.Position = {0.5, 0.5, 1.0};
	Point.Color = {0.1f, 1.0f, 0.1f};
	Point.Intensity = 2.0f;
	Point.Range = 5.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(11), std::make_unique<Durin::FPointLightSceneProxy>(Point));
	Durin::FSpotLightSceneData Spot;
	Spot.Position = {0.5, 0.5, 1.0};
	Spot.Direction = {0.0, 0.0, -1.0};
	Spot.Color = {0.1f, 0.1f, 1.0f};
	Spot.Intensity = 2.0f;
	Spot.Range = 5.0f;
	Spot.InnerConeAngle = 30.0f;
	Spot.OuterConeAngle = 45.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(12), std::make_unique<Durin::FSpotLightSceneProxy>(Spot));
	Durin::FlushRenderingCommands();
	const auto MixedLightReadback = RenderLitReadback("MixedLightSkeletalColor");
	EXPECT_EQ(ZeroLightReadback->size(), MixedLightReadback->size());
	EXPECT_NE(*ZeroLightReadback, *MixedLightReadback);
	EXPECT_TRUE(std::ranges::any_of(*MixedLightReadback, [](Durin::uint8 Value) { return Value != 0u; }));
	EXPECT_EQ(GLastCounters.HybridDeferredEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.GBufferSkeletalMeshSkippedDraws, 1u);
	EXPECT_EQ(GLastCounters.SelectedDirectionalLights, 1u);
	EXPECT_EQ(GLastCounters.SelectedPointLights, 1u);
	EXPECT_EQ(GLastCounters.SelectedSpotLights, 1u);
	EXPECT_EQ(GLastCounters.ShadowSelectedLights, 1u);
	EXPECT_EQ(GLastCounters.ShadowValidReceiverViews, 1u);
	EXPECT_EQ(GLastCounters.ShadowResourceSuccesses, 1u);
	EXPECT_EQ(GLastCounters.ShadowTargetLogicalBytes, Durin::DirectionalShadowLogicalBytes);
	EXPECT_EQ(GLastCounters.ShadowPreparedSkeletalMeshCasters, 1u);
	EXPECT_EQ(GLastCounters.ShadowAttemptedDraws, 2u);
	EXPECT_EQ(GLastCounters.ShadowSuccessfulDraws, 2u);
	EXPECT_EQ(GLastCounters.UploadedSkeletalPalettes, 1u);
	EXPECT_EQ(GLastCounters.RequestedSkeletalPaletteUploads, 2u);
	EXPECT_EQ(GLastCounters.ReusedSkeletalPalettes, 1u);

	auto OffscreenPose = std::make_shared<Durin::FSkeletalPosePalette>(*Pose);
	Scene.UpdatePrimitiveTransform(
		Durin::FPrimitiveSceneId(1),
		glm::translate(
			Durin::FMatrix(1.0), Durin::FVector3(2.0, 0.0, 0.0)
		)
	);
	Scene.AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(2),
		std::make_unique<Durin::FSkeletalMeshSceneProxy>(
			Complete.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{
				Opaque, Masked, Translucent},
			1, OffscreenPose
		),
		glm::translate(
			Durin::FMatrix(1.0), Durin::FVector3(2.0, 0.0, 5.0)
		)
	);
	Durin::FlushRenderingCommands();
	const auto OffscreenCasterReadback = RenderLitReadback(
		"OffscreenSkeletalCasterColor", false,
		Durin::EDirectionalShadowCandidate::SingleMap,
		Durin::EViewVisibilityMode::Normal,
		MakePerspectiveProjection(90.0, 2.0, 1.0, 11.0)
	);
	EXPECT_FALSE(OffscreenCasterReadback->empty());
	EXPECT_EQ(GLastCounters.PreparedSkeletalMeshPrimitives, 1u);
	EXPECT_EQ(GLastCounters.ShadowPreparedSkeletalMeshCasters, 2u);
	EXPECT_EQ(GLastCounters.UploadedSkeletalPalettes, 2u);
	EXPECT_EQ(GLastCounters.RequestedSkeletalPaletteUploads, 3u);
	EXPECT_EQ(GLastCounters.ReusedSkeletalPalettes, 1u);
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(2));
	Scene.UpdatePrimitiveTransform(
		Durin::FPrimitiveSceneId(1), Durin::FMatrix(1.0)
	);
	Durin::FlushRenderingCommands();
	if (std::getenv("DURIN_RUN_LIGHTING_PROFILE") != nullptr)
	{
		Scene.RemoveLight(Durin::FLightSceneId(11));
		Scene.RemoveLight(Durin::FLightSceneId(12));
		Scene.UpdatePrimitiveTransform(
			Durin::FPrimitiveSceneId(1),
			glm::translate(Durin::FMatrix(1.0), Durin::FVector3(-1.0, -1.0, 0.0))
				* glm::scale(Durin::FMatrix(1.0), Durin::FVector3(4.0, 4.0, 1.0))
		);
		Durin::FlushRenderingCommands();
		auto ProfileSceneColor = [&](const char* TargetName) {
			constexpr Durin::uint32 WarmupFrames = 10;
			constexpr Durin::uint32 MeasuredFrames = 120;
			std::vector<Durin::FGPUTimingQueryRHIRef> Queries;
			GSceneColorTimingQueries = &Queries;
			Durin::SetSceneColorTimingQuerySink(CaptureSceneColorTiming);
			Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
				[&Renderer, &Scene, TargetName](
					Durin::FRHICommandListImmediate& CommandList
				) {
					const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
										  TargetName, 1920, 1080,
										  Durin::EPixelFormat::SRGBA8_UNORM
					)
										  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
					Durin::FTextureRHIRef Target =
						Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Target, nullptr);
					Durin::FSceneView View;
					View.ViewProjectionMatrix = Durin::FMatrix(1.0);
					View.ViewportWidth = 1920;
					View.ViewportHeight = 1080;
					View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
					View.Settings.Mode.VisibilityMode =
						Durin::EViewVisibilityMode::FrustumCullingDisabled;
					for (Durin::uint32 Frame = 0;
						 Frame < WarmupFrames + MeasuredFrames; ++Frame)
					{
						Durin::GRenderFrameCounterRenderThread++;
						Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}), Durin::ERenderViewResult::Success);
						Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					}
				}
			);
			Durin::FlushRenderingCommands();
			Durin::SetSceneColorTimingQuerySink(nullptr);
			GSceneColorTimingQueries = nullptr;
			for (Durin::uint32 Attempt = 0; Attempt < 100; ++Attempt)
			{
				const bool bReady = Queries.size() == WarmupFrames + MeasuredFrames
									&& std::ranges::all_of(Queries, [](const auto& Query) {
										   return Query->GetResult().State
												  == Durin::ERHIGPUTimingResultState::Ready;
									   });
				if (bReady) break;
				Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
					[](Durin::FRHICommandListImmediate& CommandList) {
						Durin::GRenderFrameCounterRenderThread++;
						Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					}
				);
				Durin::FlushRenderingCommands();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			EXPECT_EQ(Queries.size(), WarmupFrames + MeasuredFrames);
			std::vector<Durin::uint64> Durations;
			for (size_t Index = WarmupFrames; Index < Queries.size(); ++Index)
			{
				const Durin::FRHIGPUTimingResult Result = Queries[Index]->GetResult();
				EXPECT_EQ(Result.State, Durin::ERHIGPUTimingResultState::Ready);
				if (Result.State == Durin::ERHIGPUTimingResultState::Ready)
					Durations.push_back(Result.DurationNanoseconds);
			}
			std::ranges::sort(Durations);
			EXPECT_EQ(Durations.size(), MeasuredFrames);
			return Durations.empty() ? Durin::uint64(0) : Durations[Durations.size() / 2];
		};
		const Durin::uint64 DirectionalMedian =
			ProfileSceneColor("DirectionalLightingProfile");
		for (Durin::uint64 Id = 20; Id < 24; ++Id)
		{
			if ((Id & 1u) == 0)
			{
				auto Data = Point;
				Data.Position.x += static_cast<double>(
									   static_cast<Durin::int64>(Id) - 24
								   )
								   * 0.1;
				Scene.AddOrReplaceLight(Durin::FLightSceneId(Id), std::make_unique<Durin::FPointLightSceneProxy>(Data));
			}
			else
			{
				auto Data = Spot;
				Data.Position.y += static_cast<double>(
									   static_cast<Durin::int64>(Id) - 24
								   )
								   * 0.1;
				Scene.AddOrReplaceLight(Durin::FLightSceneId(Id), std::make_unique<Durin::FSpotLightSceneProxy>(Data));
			}
		}
		Durin::FlushRenderingCommands();
		const Durin::uint64 MultiLightMedian =
			ProfileSceneColor("MultiLightingProfile");
		const Durin::uint64 Incremental = MultiLightMedian > DirectionalMedian ? MultiLightMedian - DirectionalMedian : 0;
		std::cout << "Lighting profile: directional=" << DirectionalMedian
				  << " ns, 1+4=" << MultiLightMedian << " ns, incremental="
				  << Incremental << " ns\n";
		EXPECT_LE(Incremental, 1'000'000u);

		constexpr Durin::uint32 ShadowWarmupFrames = 30;
		constexpr Durin::uint32 ShadowMeasuredFrames = 120;
		auto ProfileShadowTier = [&](
									 bool bEnabled,
									 Durin::EDirectionalShadowDiagnosticMode DiagnosticMode,
									 Durin::EDirectionalShadowFilterQuality FilterQuality,
									 Durin::EDirectionalShadowCandidate Candidate,
									 const char* TargetName
								 ) {
			Directional.bCastShadows = bEnabled;
			Scene.AddOrReplaceLight(Durin::FLightSceneId(10), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
			Durin::FlushRenderingCommands();
			std::vector<Durin::FGPUTimingQueryRHIRef> SceneQueries;
			std::vector<Durin::FGPUTimingQueryRHIRef> ShadowQueries;
			GSceneColorTimingQueries = &SceneQueries;
			GShadowDepthTimingQueries = &ShadowQueries;
			Durin::SetSceneColorTimingQuerySink(CaptureSceneColorTiming);
			Durin::SetShadowDepthTimingQuerySink(CaptureShadowDepthTiming);
			Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
				[&Renderer, &Scene, DiagnosticMode, FilterQuality, Candidate, TargetName](
					Durin::FRHICommandListImmediate& CommandList
				) {
					const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
										  TargetName, 1920, 1080, Durin::EPixelFormat::SRGBA8_UNORM
					)
										  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
					Durin::FTextureRHIRef Target =
						Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Target, nullptr);
					Durin::FSceneView View;
					View.ProjectionMatrix = Durin::FMatrix(0.0);
					View.ProjectionMatrix[1][0] = 0.5625;
					View.ProjectionMatrix[2][1] = -1.0;
					View.ProjectionMatrix[0][2] = 257.0 / 256.0;
					View.ProjectionMatrix[3][2] = -257.0 / 256.0;
					View.ProjectionMatrix[0][3] = 1.0;
					View.ViewProjectionMatrix = View.ProjectionMatrix;
					View.ViewportWidth = 1920;
					View.ViewportHeight = 1080;
					View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
					View.Settings.DirectionalShadow.DiagnosticMode = DiagnosticMode;
					View.Settings.DirectionalShadow.FilterQuality = FilterQuality;
					View.Settings.DirectionalShadow.Candidate = Candidate;
					View.Settings.Mode.VisibilityMode =
						Durin::EViewVisibilityMode::FrustumCullingDisabled;
					for (Durin::uint32 Frame = 0;
						 Frame < ShadowWarmupFrames + ShadowMeasuredFrames; ++Frame)
					{
						Durin::GRenderFrameCounterRenderThread++;
						Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}), Durin::ERenderViewResult::Success);
						Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					}
				}
			);
			Durin::FlushRenderingCommands();
			Durin::SetSceneColorTimingQuerySink(nullptr);
			Durin::SetShadowDepthTimingQuerySink(nullptr);
			GSceneColorTimingQueries = nullptr;
			GShadowDepthTimingQueries = nullptr;
			for (Durin::uint32 Attempt = 0; Attempt < 100; ++Attempt)
			{
				const bool bSceneReady = SceneQueries.size()
											 == ShadowWarmupFrames + ShadowMeasuredFrames
										 && std::ranges::all_of(SceneQueries, [](const auto& Query) {
												return Query->GetResult().State
													   == Durin::ERHIGPUTimingResultState::Ready;
											});
				const bool bShadowReady = !bEnabled
										  || (ShadowQueries.size()
												  == ShadowWarmupFrames + ShadowMeasuredFrames
											  && std::ranges::all_of(ShadowQueries, [](const auto& Query) {
													 return Query->GetResult().State
															== Durin::ERHIGPUTimingResultState::Ready;
												 }));
				if (bSceneReady && bShadowReady) break;
				Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
					[](Durin::FRHICommandListImmediate& CommandList) {
						Durin::GRenderFrameCounterRenderThread++;
						Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					}
				);
				Durin::FlushRenderingCommands();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			auto Median = [ShadowMeasuredFrames](const auto& Queries) {
				std::vector<Durin::uint64> Values;
				for (size_t Index = ShadowWarmupFrames;
					 Index < Queries.size(); ++Index)
					if (const auto Result = Queries[Index]->GetResult();
						Result.State == Durin::ERHIGPUTimingResultState::Ready)
						Values.push_back(Result.DurationNanoseconds);
				std::ranges::sort(Values);
				EXPECT_EQ(Values.size(), ShadowMeasuredFrames);
				return Values.empty() ? Durin::uint64(0) : Values[Values.size() / 2];
			};
			return std::pair{Median(SceneQueries), bEnabled ? Median(ShadowQueries) : Durin::uint64(0)};
		};
		const auto DisabledShadow = ProfileShadowTier(
			false, Durin::EDirectionalShadowDiagnosticMode::Lit,
			Durin::EDirectionalShadowFilterQuality::Low,
			Durin::EDirectionalShadowCandidate::SingleMap,
			"ShadowDisabledProfile"
		);
		const auto SingleMediumShadow = ProfileShadowTier(
			true, Durin::EDirectionalShadowDiagnosticMode::Lit,
			Durin::EDirectionalShadowFilterQuality::Medium,
			Durin::EDirectionalShadowCandidate::SingleMap,
			"ShadowSingleMediumProfile"
		);
		const auto LowShadow = ProfileShadowTier(
			true, Durin::EDirectionalShadowDiagnosticMode::Lit,
			Durin::EDirectionalShadowFilterQuality::Low,
			Durin::EDirectionalShadowCandidate::ThreeCascades,
			"ShadowLowProfile"
		);
		const auto MediumShadow = ProfileShadowTier(
			true, Durin::EDirectionalShadowDiagnosticMode::Lit,
			Durin::EDirectionalShadowFilterQuality::Medium,
			Durin::EDirectionalShadowCandidate::ThreeCascades,
			"ShadowMediumProfile"
		);
		const auto HighShadow = ProfileShadowTier(
			true, Durin::EDirectionalShadowDiagnosticMode::Lit,
			Durin::EDirectionalShadowFilterQuality::High,
			Durin::EDirectionalShadowCandidate::ThreeCascades,
			"ShadowHighProfile"
		);
		const auto DiagnosticShadow = ProfileShadowTier(
			true, Durin::EDirectionalShadowDiagnosticMode::FilterDifference,
			Durin::EDirectionalShadowFilterQuality::Medium,
			Durin::EDirectionalShadowCandidate::ThreeCascades,
			"ShadowFilterDiagnosticProfile"
		);
		const Durin::uint64 SingleCombined =
			SingleMediumShadow.first + SingleMediumShadow.second;
		const Durin::uint64 CandidateCombined =
			MediumShadow.first + MediumShadow.second;
		const Durin::uint64 CascadeIncrement = CandidateCombined > SingleCombined ? CandidateCombined - SingleCombined : 0;
		const Durin::uint64 SceneIncrement = LowShadow.first
													 > DisabledShadow.first ?
												 LowShadow.first - DisabledShadow.first :
												 0;
		const Durin::uint64 CombinedIncrement =
			SceneIncrement + LowShadow.second;
		const Durin::uint64 MediumSceneIncrement =
			MediumShadow.first > LowShadow.first ? MediumShadow.first - LowShadow.first : 0;
		const Durin::uint64 HighSceneIncrement =
			HighShadow.first > LowShadow.first ? HighShadow.first - LowShadow.first : 0;
		const Durin::uint64 MediumShadowDepthRegression =
			MediumShadow.second > LowShadow.second ? MediumShadow.second - LowShadow.second : 0;
		const Durin::uint64 HighShadowDepthRegression =
			HighShadow.second > LowShadow.second ? HighShadow.second - LowShadow.second : 0;
		const Durin::uint64 DiagnosticIncrement = DiagnosticShadow.first
														  > MediumShadow.first ?
													  DiagnosticShadow.first - MediumShadow.first :
													  0;
		std::cout << "Directional shadow profile: disabled-scene="
				  << DisabledShadow.first << " ns, enabled-scene="
				  << LowShadow.first << " ns, low-shadow-depth="
				  << LowShadow.second << " ns, combined-increment="
				  << CombinedIncrement << " ns, logical-bytes="
				  << Durin::DirectionalShadowLogicalBytes << ", backend-bytes="
				  << GLastCounters.ShadowTargetBackendBytes
				  << ", medium-scene=" << MediumShadow.first
				  << " ns, medium-increment=" << MediumSceneIncrement
				  << " ns, high-scene=" << HighShadow.first
				  << " ns, high-increment=" << HighSceneIncrement
				  << " ns, medium-shadow-depth=" << MediumShadow.second
				  << " ns, high-shadow-depth=" << HighShadow.second
				  << " ns, diagnostic-scene=" << DiagnosticShadow.first
				  << " ns, diagnostic-increment=" << DiagnosticIncrement
				  << " ns, single-medium-combined=" << SingleCombined
				  << " ns, cascade-medium-combined=" << CandidateCombined
				  << " ns, cascade-increment=" << CascadeIncrement << " ns\n";
		EXPECT_LE(CombinedIncrement, 2'000'000u);
		EXPECT_LE(MediumSceneIncrement, 200'000u);
		EXPECT_LE(HighSceneIncrement, 400'000u);
		EXPECT_LE(MediumShadowDepthRegression, 20'000u);
		EXPECT_LE(HighShadowDepthRegression, 20'000u);
		EXPECT_LE(CascadeIncrement, 1'000'000u);
	}
	auto TranslatedPose = std::make_shared<Durin::FSkeletalPosePalette>(*Pose);
	TranslatedPose->Revision = 2;
	TranslatedPose->Matrices[1] = glm::scale(
		Durin::FMatrix4f(1.0f), Durin::FVector3f(0.5f, 1.5f, 1.0f)
	);
	TranslatedPose->LocalBounds = Durin::FBox(
		Pose->LocalBounds.Min * 0.5, Pose->LocalBounds.Max * 0.5
	);
	Scene.UpdateSkeletalMeshDynamicData(
		Durin::FPrimitiveSceneId(1), TranslatedPose
	);
	Durin::FlushRenderingCommands();
	auto TranslatedReadback = std::make_shared<std::vector<Durin::uint8>>();
	auto AuxiliaryReadback = std::make_shared<std::vector<Durin::uint8>>();
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&Renderer, &Scene, TranslatedReadback, AuxiliaryReadback](
			Durin::FRHICommandListImmediate& CommandList
		) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "TranslatedSkeletalMeshValidationColor", 33, 33,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource | Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 33;
			View.ViewportHeight = 33;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			(void)Renderer.RenderView(
				CommandList, &Scene, View, Target, false, {}
			);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Target, 0, 0, *TranslatedReadback
			));

			const auto AuxiliaryDesc = Durin::FRHITextureCreateDesc::Create2D(
										   "AuxiliarySkeletalMeshValidationColor", 48, 27,
										   Durin::EPixelFormat::SRGBA8_UNORM
			)
										   .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource | Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef AuxiliaryTarget =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, AuxiliaryDesc);
			ASSERT_NE(AuxiliaryTarget, nullptr);
			Durin::FSceneView AuxiliaryView;
			AuxiliaryView.ViewProjectionMatrix = Durin::FMatrix(1.0);
			AuxiliaryView.ViewportWidth = 48;
			AuxiliaryView.ViewportHeight = 27;
			AuxiliaryView.AspectRatioConstraint = 16.0f / 9.0f;
			AuxiliaryView.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
			AuxiliaryView.Settings.Mode.RasterMode = Durin::ERasterMode::Wireframe;
			AuxiliaryView.Settings.PostProcess.bEnableFXAA = false;
			AuxiliaryView.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			(void)Renderer.RenderView(CommandList, &Scene, AuxiliaryView, AuxiliaryTarget, false, {});
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, AuxiliaryTarget, 0, 0, *AuxiliaryReadback
			));
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		}
	);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(TranslatedReadback->size(), Readback->size());
	EXPECT_NE(*TranslatedReadback, *Readback);
	EXPECT_EQ(AuxiliaryReadback->size(), 48u * 27u * 4u);
	EXPECT_NE(AuxiliaryReadback->size(), TranslatedReadback->size());

	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(1));
	Durin::FSplineMeshRenderDynamicData SplineData{
		.Params = {}, .LocalBounds = Durin::FBox({-1.0, -1.0, -0.1}, {1.0, 1.0, 0.1}), .Revision = 1
	};
	SplineData.Params.StartTangent = {1.5, 0.0, 0.0};
	SplineData.Params.EndPosition = {0.7, 0.5, 0.0};
	SplineData.Params.EndTangent = {0.0, 1.5, 0.0};
	SplineData.Params.SourceForwardMin = -0.8;
	SplineData.Params.SourceForwardMax = 0.8;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FSplineMeshSceneProxy>(SplineSource.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}, 1, SplineData), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	auto SplineReadback = std::make_shared<std::vector<Durin::uint8>>();
	std::array<std::vector<Durin::uint8>, 4> SplineGBufferPixels;
	for (size_t Index = 0; Index < GGBufferPixels.size(); ++Index)
		GGBufferPixels[Index] = &SplineGBufferPixels[Index];
	Durin::SetGBufferCaptureSink(CaptureGBuffer);
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&Renderer, &Scene, SplineReadback](Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "SplineMeshValidationColor", 33, 33, Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource | Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Target = Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 33;
			View.ViewportHeight = 33;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
			Durin::FSceneViewRenderOptions QualificationOptions;
			QualificationOptions.bEnableGBufferQualification = true;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, QualificationOptions), Durin::ERenderViewResult::Success);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(CommandList, Target, 0, 0, *SplineReadback));
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		}
	);
	Durin::FlushRenderingCommands();
	Durin::SetGBufferCaptureSink(nullptr);
	GGBufferPixels.fill(nullptr);
	ValidateCapturedGBuffer(SplineGBufferPixels);
	EXPECT_EQ(GLastCounters.VisibleSplineMeshCandidates, 1u);
	EXPECT_EQ(GLastCounters.PreparedSplineMeshPrimitives, 1u);
	EXPECT_EQ(GLastCounters.PreparedSplineMeshSections, 3u);
	EXPECT_EQ(GLastCounters.PreparedSplineMeshTriangles, 3u);
	EXPECT_EQ(GLastCounters.RetainedSplineMeshDeformationBytes, sizeof(Durin::FSplineMeshRenderDynamicData));
	EXPECT_EQ(GLastCounters.GBufferSplineMeshAttemptedDraws, 2u);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshSuccessfulDraws, 2u);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshRejectedDraws, 0u);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshSkippedDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferStaticMeshAttemptedDraws, 0u);
	EXPECT_EQ(SplineReadback->size(), 33u * 33u * 4u);
	EXPECT_TRUE(std::ranges::any_of(*SplineReadback, [](Durin::uint8 Value) { return Value != 0; }));
	auto CaptureSplineParity = [&](std::string Name,
								   bool bEnableGBufferQualification = false) {
		auto Result = std::make_shared<std::vector<Durin::uint8>>();
		Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
			[&Renderer, &Scene, Result, Name = std::move(Name),
			 bEnableGBufferQualification](Durin::FRHICommandListImmediate& CommandList) {
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
									  Name.c_str(), 33, 33, Durin::EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource | Durin::ETextureCreateFlags::CPUReadback);
				Durin::FTextureRHIRef Target = Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				Durin::FSceneView View;
				View.ViewProjectionMatrix = Durin::FMatrix(1.0);
				View.ViewportWidth = 33;
				View.ViewportHeight = 33;
				View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
				View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
				Durin::FSceneViewRenderOptions RenderOptions;
				RenderOptions.bEnableGBufferQualification =
					bEnableGBufferQualification;
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, RenderOptions), Durin::ERenderViewResult::Success);
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(CommandList, Target, 0, 0, *Result));
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		return Result;
	};
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FSplineMeshSceneProxy>(SplineSource.get(), std::vector<Durin::FMaterialRenderProxyRef>{Unlit, Masked, Translucent}, 1, SplineData), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	const auto UnlitForwardReadback = CaptureSplineParity("UnlitSplineForwardColor");
	const auto UnlitGBufferReadback = CaptureSplineParity(
		"UnlitSplineGBufferColor", true
	);
	EXPECT_EQ(*UnlitGBufferReadback, *UnlitForwardReadback);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshAttemptedDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshRejectedDraws, 0u);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshSkippedDraws, 2u);
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FSplineMeshSceneProxy>(SplineSource.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}, 1, SplineData), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	auto CpuSplineSource = MakeSplineSourceRenderData();
	{
		auto& LOD = CpuSplineSource->LODResources[0];
		const auto SourcePositions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
		std::vector<Durin::FVector3f> CpuPositions;
		std::vector<Durin::FVector3f> CpuNormals;
		std::vector<Durin::FVector4f> CpuTangents;
		CpuPositions.reserve(SourcePositions.size());
		CpuNormals.reserve(SourcePositions.size());
		CpuTangents.reserve(SourcePositions.size());
		for (const Durin::FVector3f& SourcePosition : SourcePositions)
		{
			CpuPositions.emplace_back(Durin::FSplineMeshDeformer::DeformPosition(
				SplineData.Params, Durin::FVector3(SourcePosition)
			));
			const Durin::FSplineMeshTangentBasis Basis = Durin::FSplineMeshDeformer::DeformTangentBasis(
				SplineData.Params, Durin::FVector3(SourcePosition), {0.0, 0.0, 1.0},
				{1.0, 0.0, 0.0}, 1.0
			);
			CpuNormals.emplace_back(Basis.Normal);
			CpuTangents.emplace_back(Durin::FVector3f(Basis.Tangent), static_cast<float>(Basis.Handedness));
		}
		LOD.VertexBuffers.PositionVertexBuffer.Init(std::move(CpuPositions));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::move(CpuNormals), std::move(CpuTangents)
		);
		LOD.LocalBounds = SplineData.LocalBounds;
		CpuSplineSource->RecalculateBounds();
	}
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&CpuSplineSource](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(CpuSplineSource->InitResources(CommandList));
		}
	);
	Durin::FlushRenderingCommands();
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FStaticMeshSceneProxy>(CpuSplineSource.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}, 1), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	const auto CpuCurvedReadback = CaptureSplineParity("CpuCurvedSplineMeshColor");
	EXPECT_EQ(*SplineReadback, *CpuCurvedReadback);
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FSplineMeshSceneProxy>(SplineSource.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}, 1, SplineData), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	auto IdentityData = SplineData;
	IdentityData.Revision = 2;
	IdentityData.Params.StartPosition = {-0.8, 0.0, 0.0};
	IdentityData.Params.StartTangent = {1.6, 0.0, 0.0};
	IdentityData.Params.EndPosition = {0.8, 0.0, 0.0};
	IdentityData.Params.EndTangent = {1.6, 0.0, 0.0};
	Scene.UpdateSplineMeshDynamicData(Durin::FPrimitiveSceneId(2), IdentityData);
	Durin::FlushRenderingCommands();
	const auto IdentitySplineReadback = CaptureSplineParity("IdentitySplineMeshColor");
	EXPECT_EQ(GLastCounters.AcceptedSplineMeshDynamicUpdates, 1u);
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FStaticMeshSceneProxy>(SplineSource.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}, 1), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	const auto StaticParityReadback = CaptureSplineParity("StaticMeshParityColor");
	EXPECT_EQ(*IdentitySplineReadback, *StaticParityReadback);
	EXPECT_NE(*SplineReadback, *IdentitySplineReadback);
	if (std::getenv("DURIN_RUN_SPLINE_PROFILE") != nullptr)
	{
		auto RoadSource = MakeSplineRoadRenderData();
		Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
			[&RoadSource](Durin::FRHICommandListImmediate& CommandList) {
				ASSERT_TRUE(RoadSource->InitResources(CommandList));
			}
		);
		Durin::FlushRenderingCommands();
		Durin::FScenePtr ProfileSceneOwner = Renderer.CreateScene();
		auto& ProfileScene =
			static_cast<Durin::FScene&>(*ProfileSceneOwner);
		auto SegmentTransform = [](Durin::uint32 Index) {
			const double X = -0.9 + static_cast<double>(Index % 8) * 0.24;
			const double Y = -0.9 + static_cast<double>(Index / 8) * 0.48;
			return glm::translate(Durin::FMatrix(1.0), Durin::FVector3(X, Y, 0.0))
				   * glm::scale(Durin::FMatrix(1.0), Durin::FVector3(0.18, 0.18, 1.0));
		};
		for (Durin::uint32 Index = 0; Index < 32; ++Index)
			ProfileScene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(100 + Index), std::make_unique<Durin::FStaticMeshSceneProxy>(RoadSource.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1), SegmentTransform(Index));
		Durin::FlushRenderingCommands();
		auto ProfileP95 = [&](const char* TargetName) {
			constexpr Durin::uint32 WarmupFrames = 10;
			constexpr Durin::uint32 MeasuredFrames = 120;
			std::vector<Durin::FGPUTimingQueryRHIRef> Queries;
			GSceneColorTimingQueries = &Queries;
			Durin::SetSceneColorTimingQuerySink(CaptureSceneColorTiming);
			Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
				[&Renderer, &ProfileScene, TargetName](Durin::FRHICommandListImmediate& CommandList) {
					const auto Desc = Durin::FRHITextureCreateDesc::Create2D(TargetName, 1920, 1080, Durin::EPixelFormat::SRGBA8_UNORM).SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
					Durin::FTextureRHIRef Target = Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
					Durin::FSceneView View;
					View.ViewProjectionMatrix = Durin::FMatrix(1.0);
					View.ViewportWidth = 1920;
					View.ViewportHeight = 1080;
					View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
					View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
					for (Durin::uint32 Frame = 0; Frame < WarmupFrames + MeasuredFrames; ++Frame)
					{
						Durin::GRenderFrameCounterRenderThread++;
						Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						EXPECT_EQ(Renderer.RenderView(CommandList, &ProfileScene, View, Target, false, {}), Durin::ERenderViewResult::Success);
						Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					}
				}
			);
			Durin::FlushRenderingCommands();
			Durin::SetSceneColorTimingQuerySink(nullptr);
			GSceneColorTimingQueries = nullptr;
			for (Durin::uint32 Attempt = 0; Attempt < 200; ++Attempt)
			{
				if (Queries.size() == WarmupFrames + MeasuredFrames
					&& std::ranges::all_of(Queries, [](const auto& Query) { return Query->GetResult().State == Durin::ERHIGPUTimingResultState::Ready; })) break;
				Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
					[](Durin::FRHICommandListImmediate& CommandList) {
						Durin::GRenderFrameCounterRenderThread++;
						Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
						Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					}
				);
				Durin::FlushRenderingCommands();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			std::vector<Durin::uint64> Durations;
			for (size_t Index = WarmupFrames; Index < Queries.size(); ++Index)
				if (const auto Result = Queries[Index]->GetResult();
					Result.State == Durin::ERHIGPUTimingResultState::Ready)
					Durations.push_back(Result.DurationNanoseconds);
			std::ranges::sort(Durations);
			EXPECT_EQ(Durations.size(), MeasuredFrames);
			return Durations.size() == MeasuredFrames ? Durations[113] : Durin::uint64(0);
		};
		const Durin::uint64 StaticP95 = ProfileP95("StaticRoadProfile");
		Durin::FSplineMeshRenderDynamicData RoadSpline{
			.Params = {}, .LocalBounds = Durin::FBox({-0.1, -0.6, -0.2}, {1.1, 0.8, 0.4}), .Revision = 1
		};
		RoadSpline.Params.StartTangent = {0.8, 0.5, 0.2};
		RoadSpline.Params.EndPosition = {1.0, 0.25, 0.1};
		RoadSpline.Params.EndTangent = {0.8, -0.2, 0.3};
		RoadSpline.Params.SourceForwardMin = 0.0;
		RoadSpline.Params.SourceForwardMax = 1.0;
		for (Durin::uint32 Index = 0; Index < 32; ++Index)
			ProfileScene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(100 + Index), std::make_unique<Durin::FSplineMeshSceneProxy>(RoadSource.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 1, RoadSpline), SegmentTransform(Index));
		Durin::FlushRenderingCommands();
		const Durin::uint64 SplineP95 = ProfileP95("SplineRoadProfile");
		const Durin::uint64 Delta = SplineP95 > StaticP95 ? SplineP95 - StaticP95 : 0;
		std::cout << "Spline profile: static_p95=" << StaticP95
				  << " ns, spline_p95=" << SplineP95 << " ns, delta=" << Delta << " ns\n";
		EXPECT_LE(Delta, 350'000u);
		ProfileSceneOwner.reset();
		Durin::FlushRenderingCommands();
		Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
			[&RoadSource](Durin::FRHICommandListImmediate&) { RoadSource->ReleaseResources(); }
		);
		Durin::FlushRenderingCommands();
	}

	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(2));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&Complete, &SplineSource, &CpuSplineSource](Durin::FRHICommandListImmediate&) {
			Complete->ReleaseResources();
			SplineSource->ReleaseResources();
			CpuSplineSource->ReleaseResources();
		}
	);
	Durin::FlushRenderingCommands();
	RendererLifecycle.Shutdown();
	Durin::SetViewRenderCounterSink(nullptr);
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
