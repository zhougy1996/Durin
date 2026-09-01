#include "CoreGlobals.h"
#include "VulkanEngineTestSupport.h"
#include "DynamicRHI.h"
#include "Asset/AssetCompilingManager.h"
#include "AssetRegistry/Catalog.h"
#include "EngineTestSupport.h"
#include "LightSceneTestSupport.h"
#include "LightSceneTestSupport.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/SkeletalMeshSceneProxy.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/TerrainSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "Math/Operations.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RendererModule.h"
#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/DirectionalShadowRenderer.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneVisibility.h"
#include "RenderingThread.h"
#include "SceneTestAccess.h"
#include "SceneView.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Terrain/TerrainHeightmap.h"
#include <vulkan/vulkan.hpp>
#include "VulkanDynamicRHI.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ranges>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
namespace
{
	constexpr uint32 TimingWidth = 1920;
	constexpr uint32 TimingHeight = 1080;
	constexpr uint32 WarmupFrames = 30;
	constexpr uint32 MeasuredFrames = 120;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GSceneTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GGBufferTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GDeferredTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GRetainedOpaqueTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GVolumetricCloudTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GSortedTranslucencyTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GPostProcessTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GShadowTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GContactTimingQueries = nullptr;
	Durin::FContactShadowVisibilityRenderer::ERoute GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::FactorOne;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GGroundTruthAmbientOcclusionTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GGroundTruthAmbientOcclusionFilterTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GGroundTruthAmbientOcclusionResolveTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GGroundTruthAmbientOcclusionFeatureTimingQueries = nullptr;
	Durin::FByteArray* GGroundTruthAmbientOcclusionPixels = nullptr;
	Durin::FByteArray* GGroundTruthAmbientOcclusionFilteredPixels = nullptr;
	Durin::FByteArray* GSpecularAASurfacePixels = nullptr;
	Durin::FViewRenderTelemetry GLastTelemetry;

	class FGBufferQualificationEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
			MountRegistry = std::make_unique<
				Durin::Testing::FScopedMountRegistryFixture>();
			ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
			ASSERT_TRUE(Durin::RefreshAssetRegistry());
		}

		auto TearDown() -> void override
		{
			Durin::ShutdownAssetCompilingManager();
			MountRegistry.reset();
		}

	private:
		std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture>
			MountRegistry;
	};

	[[maybe_unused]] testing::Environment* GGBufferQualificationEnvironment =
		testing::AddGlobalTestEnvironment(new FGBufferQualificationEnvironment);

	auto MakeMaterial(
		const char* Name,
		Durin::EMaterialBlendMode BlendMode,
		Durin::EMaterialShadingModel ShadingModel,
		const Durin::FVector3& BaseColor,
		float Metallic = 0.0f,
		float Roughness = 0.5f,
		float Opacity = 1.0f) -> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, Name);
		if (Material == nullptr)
		{
			ADD_FAILURE() << "Failed to create GBuffer qualification material."
				<< Name;
			return nullptr;
		}
		Durin::FMaterialProgramValidationResult Validation;
		if (!Material->SetMaterialProgram(
				Durin::MakeCanonicalMaterialProgram(), Validation))
		{
			ADD_FAILURE() << "Failed to install GBuffer qualification material "
				"program: " << Name;
			return nullptr;
		}
		EXPECT_TRUE(Material->SetStaticProperties(
			Durin::FMaterialStaticProperties{
				.BlendMode = BlendMode,
				.ShadingModel = ShadingModel,
				.bTwoSided = true}));
		EXPECT_TRUE(Material->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(), BaseColor));
		EXPECT_TRUE(Material->SetScalarParameterValue(
			Durin::MaterialParameters::MetallicName(), Metallic));
		EXPECT_TRUE(Material->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), Roughness));
		if (BlendMode == Durin::EMaterialBlendMode::Translucent)
			EXPECT_TRUE(Material->SetScalarParameterValue(
				Durin::MaterialParameters::OpacityName(), Opacity));
		if (Material->GetMaterialCompileStatus().State
			== Durin::EMaterialCompileState::NeverRequested)
			EXPECT_TRUE(Durin::RequestMaterialRecompile(*Material));
		Durin::DObject* Object = Material;
		Durin::FAssetCompilingManager::Get().FinishCompilationForObjects(
			std::span<Durin::DObject* const>(&Object, 1));
		if (!Material->GetMaterialCompileStatus().IsCurrent()
			|| !Material->GetAcceptedCompiledProgram())
		{
			ADD_FAILURE() << "GBuffer qualification material compilation failed: "
				<< Name;
			return nullptr;
		}
		return Material;
	}

	auto ReadColorTexture(
		Durin::FRHICommandListImmediate& CommandList,
		Durin::FRHITexture* Source,
		Durin::FByteArray& Pixels) -> void
	{
		const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
			"GBufferQualificationColorReadback", Source->GetSizeX(),
			Source->GetSizeY(), Source->GetFormat())
			.SetFlags(Durin::ETextureCreateFlags::DestinationCopy
				| Durin::ETextureCreateFlags::CPUReadback
				| Durin::ETextureCreateFlags::ShaderResource);
		Durin::FTextureRHIRef Readback =
			Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
		ASSERT_NE(Readback, nullptr);
		const Durin::FRHITextureSubresourceRange Whole{
			Durin::ERHITextureAspect::Color, 0, 1, 0, 1};
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{Source, Whole,
				Durin::ERHIAccess::GraphicsShaderRead,
				Durin::ERHIAccess::TransferRead},
			Durin::FRHITextureTransition{Readback, Whole,
				Durin::ERHIAccess::Discard,
				Durin::ERHIAccess::TransferWrite}});
		CommandList.CopyTexture(
			Source, Readback,
			std::array{Durin::FRHITextureCopyRegion{
				.Extent = {Source->GetSizeX(), Source->GetSizeY(), 1}}});
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{Source, Whole,
				Durin::ERHIAccess::TransferRead,
				Durin::ERHIAccess::GraphicsShaderRead},
			Durin::FRHITextureTransition{Readback, Whole,
				Durin::ERHIAccess::TransferWrite,
				Durin::ERHIAccess::GraphicsShaderRead}});
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			CommandList, Readback, 0, 0, Pixels));
	}

	struct FGBufferQualificationCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "GBufferQualification";
		}
	};

	auto CaptureGBufferTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GGBufferTimingQueries != nullptr)
			GGBufferTimingQueries->push_back(Query);
	}

	auto CaptureSceneTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GSceneTimingQueries != nullptr)
			GSceneTimingQueries->push_back(Query);
	}

	auto CaptureDeferredTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GDeferredTimingQueries != nullptr)
			GDeferredTimingQueries->push_back(Query);
	}

	auto CaptureRetainedOpaqueTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GRetainedOpaqueTimingQueries != nullptr)
			GRetainedOpaqueTimingQueries->push_back(Query);
	}

	auto CaptureVolumetricCloudTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GVolumetricCloudTimingQueries != nullptr)
			GVolumetricCloudTimingQueries->push_back(Query);
	}

	auto CaptureSortedTranslucencyTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GSortedTranslucencyTimingQueries != nullptr)
			GSortedTranslucencyTimingQueries->push_back(Query);
	}

	auto CapturePostProcessTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GPostProcessTimingQueries != nullptr)
			GPostProcessTimingQueries->push_back(Query);
	}

	auto CaptureShadowTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GShadowTimingQueries != nullptr)
			GShadowTimingQueries->push_back(Query);
	}

	auto CaptureContactTiming(const Durin::FGPUTimingQueryRHIRef& Query,
		Durin::FContactShadowVisibilityRenderer::ERoute Route) -> void
	{
		if (GContactTimingQueries != nullptr && Route == GExpectedContactRoute)
			GContactTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusionTiming(
		const Durin::FGPUTimingQueryRHIRef& Query
	) -> void
	{
		if (GGroundTruthAmbientOcclusionTimingQueries != nullptr)
			GGroundTruthAmbientOcclusionTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusionFilterTiming(
		const Durin::FGPUTimingQueryRHIRef& Query
	) -> void
	{
		if (GGroundTruthAmbientOcclusionFilterTimingQueries != nullptr)
			GGroundTruthAmbientOcclusionFilterTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusionResolveTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GGroundTruthAmbientOcclusionResolveTimingQueries != nullptr)
			GGroundTruthAmbientOcclusionResolveTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusionFeatureTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GGroundTruthAmbientOcclusionFeatureTimingQueries != nullptr)
			GGroundTruthAmbientOcclusionFeatureTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusion(
		Durin::FRHICommandListImmediate& CommandList,
		Durin::FRHITexture* RawVisibility,
		bool bFiltered
	) -> void
	{
		auto* Pixels = bFiltered ? GGroundTruthAmbientOcclusionFilteredPixels : GGroundTruthAmbientOcclusionPixels;
		if (Pixels == nullptr) return;
		const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
							  "GroundTruthAmbientOcclusionReadback",
							  RawVisibility->GetSizeX(), RawVisibility->GetSizeY(),
							  RawVisibility->GetFormat()
		)
							  .SetFlags(Durin::ETextureCreateFlags::DestinationCopy | Durin::ETextureCreateFlags::CPUReadback | Durin::ETextureCreateFlags::ShaderResource);
		Durin::FTextureRHIRef Readback =
			Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
		ASSERT_NE(Readback, nullptr);
		const Durin::FRHITextureSubresourceRange Whole{
			Durin::ERHITextureAspect::Color, 0, 1, 0, 1
		};
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{RawVisibility, Whole, Durin::ERHIAccess::GraphicsShaderRead, Durin::ERHIAccess::TransferRead},
			Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::Discard, Durin::ERHIAccess::TransferWrite}
		});
		CommandList.CopyTexture(RawVisibility, Readback, std::array{Durin::FRHITextureCopyRegion{.Extent = {RawVisibility->GetSizeX(), RawVisibility->GetSizeY(), 1}}});
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{RawVisibility, Whole, Durin::ERHIAccess::TransferRead, Durin::ERHIAccess::GraphicsShaderRead},
			Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead}
		});
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			CommandList, Readback, 0, 0,
			*Pixels
		));
	}

	auto CaptureTelemetry(const Durin::FViewRenderTelemetry& Telemetry) -> void
	{
		GLastTelemetry = Telemetry;
	}

	auto CaptureSpecularAAGBuffer(
		Durin::FRHICommandListImmediate& CommandList,
		Durin::FRHITexture*,
		Durin::FRHITexture*,
		Durin::FRHITexture* Surface,
		Durin::FRHITexture*,
		Durin::FRHITexture*) -> void
	{
		if (GSpecularAASurfacePixels == nullptr) return;
		const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
			"SpecularAASurfaceReadback", Surface->GetSizeX(), Surface->GetSizeY(),
			Surface->GetFormat())
			.SetFlags(Durin::ETextureCreateFlags::DestinationCopy
				| Durin::ETextureCreateFlags::CPUReadback
				| Durin::ETextureCreateFlags::ShaderResource);
		Durin::FTextureRHIRef Readback =
			Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
		ASSERT_NE(Readback, nullptr);
		const Durin::FRHITextureSubresourceRange Whole{
			Durin::ERHITextureAspect::Color, 0, 1, 0, 1};
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{Surface, Whole,
				Durin::ERHIAccess::GraphicsShaderRead,
				Durin::ERHIAccess::TransferRead},
			Durin::FRHITextureTransition{Readback, Whole,
				Durin::ERHIAccess::Discard,
				Durin::ERHIAccess::TransferWrite}});
		CommandList.CopyTexture(
			Surface, Readback,
			std::array{Durin::FRHITextureCopyRegion{
				.Extent = {Surface->GetSizeX(), Surface->GetSizeY(), 1}}});
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{Surface, Whole,
				Durin::ERHIAccess::TransferRead,
				Durin::ERHIAccess::GraphicsShaderRead},
			Durin::FRHITextureTransition{Readback, Whole,
				Durin::ERHIAccess::TransferWrite,
				Durin::ERHIAccess::GraphicsShaderRead}});
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			CommandList, Readback, 0, 0, *GSpecularAASurfacePixels));
	}

	auto MakeStaticQuad() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FStaticMeshRenderData>();
		Data->MaterialSlots = {{"Opaque", 0}};
		auto& LOD = Data->LODResources.emplace_back();
		LOD.VertexBuffers.PositionVertexBuffer.Init({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
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
		LOD.Sections.push_back({.Name = "Opaque", .FirstIndex = 0, .IndexCount = 6, .MinVertexIndex = 0, .MaxVertexIndex = 3, .MaterialSlotIndex = 0, .LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})});
		LOD.LocalBounds = LOD.Sections[0].LocalBounds;
		Data->LODVertexFactories.resize(1);
		Data->RecalculateBounds();
		return Data;
	}

	auto MakeSpecularAAQuad() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		constexpr uint32 QuadsPerAxis = 8;
		constexpr uint32 VerticesPerAxis = QuadsPerAxis + 1;
		constexpr float Extent = 0.25f;
		auto Data = std::make_unique<Durin::FStaticMeshRenderData>();
		Data->MaterialSlots = {{"Opaque", 0}};
		auto& LOD = Data->LODResources.emplace_back();
		std::vector<Durin::FVector3f> Positions;
		std::vector<Durin::FVector3f> Normals;
		std::vector<Durin::FVector4f> Tangents;
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels>
			UVs;
		Positions.reserve(VerticesPerAxis * VerticesPerAxis);
		Normals.reserve(VerticesPerAxis * VerticesPerAxis);
		Tangents.reserve(VerticesPerAxis * VerticesPerAxis);
		UVs[0].reserve(VerticesPerAxis * VerticesPerAxis);
		for (uint32 Y = 0; Y < VerticesPerAxis; ++Y)
		{
			for (uint32 X = 0; X < VerticesPerAxis; ++X)
			{
				const float U = static_cast<float>(X) / QuadsPerAxis;
				const float V = static_cast<float>(Y) / QuadsPerAxis;
				Positions.push_back({
					-Extent + 2.0f * Extent * U,
					-Extent + 2.0f * Extent * V, 0.0f});
				const float NormalX = ((X + Y) & 1u) == 0u ? -0.8f : 0.8f;
				Normals.push_back({NormalX, 0.0f, 0.6f});
				Tangents.push_back({0.0f, 1.0f, 0.0f, 1.0f});
				UVs[0].push_back({U, V});
			}
		}
		std::vector<uint32> Indices;
		Indices.reserve(QuadsPerAxis * QuadsPerAxis * 6u);
		for (uint32 Y = 0; Y < QuadsPerAxis; ++Y)
		{
			for (uint32 X = 0; X < QuadsPerAxis; ++X)
			{
				const uint32 A = Y * VerticesPerAxis + X;
				const uint32 B = A + 1u;
				const uint32 C = A + VerticesPerAxis;
				const uint32 D = C + 1u;
				Indices.insert(Indices.end(), {A, B, D, A, D, C});
			}
		}
		LOD.VertexBuffers.PositionVertexBuffer.Init(Positions);
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			Normals, Tangents);
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), static_cast<uint32>(Positions.size()), 1);
		LOD.VertexBuffers.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(Positions.size(), Durin::FVector4f(1.0f)),
			static_cast<uint32>(Positions.size()));
		LOD.IndexBuffer.Init(Indices);
		LOD.Sections.push_back({
			.Name = "Opaque", .FirstIndex = 0,
			.IndexCount = static_cast<uint32>(Indices.size()),
			.MinVertexIndex = 0,
			.MaxVertexIndex = static_cast<uint32>(Positions.size() - 1u),
			.MaterialSlotIndex = 0,
			.LocalBounds = Durin::FBox(
				{-Extent, -Extent, 0.0}, {Extent, Extent, 0.0})});
		LOD.LocalBounds = LOD.Sections[0].LocalBounds;
		LOD.NumTexCoords = 1;
		LOD.bHasColorVertexData = true;
		Data->LODVertexFactories.resize(1);
		Data->RecalculateBounds();
		return Data;
	}

	auto MakeSkeletalQuad() -> std::unique_ptr<Durin::FSkeletalMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FSkeletalMeshRenderData>();
		Data->VertexBuffers.Geometry.PositionVertexBuffer.Init({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(4, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(4, {1.0f, 0.0f, 0.0f, 1.0f})
		);
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels>
			UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), 4, 1
		);
		Data->VertexBuffers.Geometry.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(4, Durin::FVector4f(1.0f)), 4
		);
		Durin::FSkeletalMeshVertexInfluences Influence;
		Influence.BoneIndices[0] = 0;
		Influence.Weights[0] = 1.0f;
		Influence.Count = 1;
		Data->VertexBuffers.InfluenceVertexBuffer.Init(
			std::vector<Durin::FSkeletalMeshVertexInfluences>(4, Influence)
		);
		Data->IndexBuffer.Init({0, 1, 2, 0, 2, 3});
		Data->Sections.push_back({.Name = Durin::FName("Opaque"), .FirstIndex = 0, .IndexCount = 6, .MinVertexIndex = 0, .MaxVertexIndex = 3, .MaterialSlotIndex = 0, .LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})});
		Data->MaterialSlots = {Durin::FName("Opaque")};
		Data->PaletteBoneIndices = {0};
		Data->InverseBindMatrices = {Durin::FMatrix4f(1.0f)};
		Data->InfluenceBounds = {
			Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})
		};
		Data->LocalBounds = Data->Sections[0].LocalBounds;
		return Data;
	}
} // namespace

TEST(FGBufferQualificationTests, FourFamilyPassMeetsFrozenRTX3090TimingAndMemoryGates)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	auto* Vulkan = static_cast<Durin::VulkanRHI::IVulkanDynamicRHI*>(
		Durin::GDynamicRHI);
	vk::PhysicalDeviceProperties DeviceProperties{};
	Vulkan->RHIExecuteCommandBufferForBackendIntegration(
		[&Vulkan, &DeviceProperties](vk::CommandBuffer) {
			DeviceProperties = Vulkan->RHIGetVkPhysicalDevice().getProperties();
		});
	const std::string DeviceName = DeviceProperties.deviceName.data();
	const bool bNamedAdapter = DeviceName == "NVIDIA GeForce RTX 3090"
		&& vk::apiVersionMajor(DeviceProperties.apiVersion) == 1u
		&& vk::apiVersionMinor(DeviceProperties.apiVersion) == 4u
		&& vk::apiVersionPatch(DeviceProperties.apiVersion) == 325u;
	Durin::InitRenderingThread();
	Durin::FRendererModule Renderer;
	Durin::FModuleTestHarness RendererLifecycle("GBufferQualificationTest");
	RendererLifecycle.Start(Renderer);
	Durin::SetViewRenderTelemetrySink(CaptureTelemetry);

	auto StaticQuad = MakeStaticQuad();
	auto SpecularAAQuad = MakeSpecularAAQuad();
	auto SkeletalQuad = MakeSkeletalQuad();
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(StaticQuad->InitResources(CommandList));
			ASSERT_TRUE(SpecularAAQuad->InitResources(CommandList));
			ASSERT_TRUE(SkeletalQuad->InitResources(CommandList));
		}
	);
	Durin::FlushRenderingCommands();

	auto* MaterialObject = MakeMaterial(
		"GBufferQualificationLit", Durin::EMaterialBlendMode::Opaque,
		Durin::EMaterialShadingModel::Lit, {0.7, 0.4, 0.2});
	ASSERT_NE(MaterialObject, nullptr);
	auto Material = MaterialObject->GetMaterialRenderProxy();
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner SceneOwner;

	Durin::FScene& Scene = *SceneOwner;
	auto Translate = [](double X, double Y) {
		return Durin::Math::TranslationMatrix(Durin::FVector3{X, Y, 0.0});
	};
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(1), std::make_unique<Durin::FStaticMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1), Translate(-1.0, -1.0));
	Durin::FSplineMeshRenderDynamicData SplineData{
		.Params = {},
		.LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}),
		.Revision = 1
	};
	SplineData.Params.EndPosition = {1.0, 0.0, 0.0};
	SplineData.Params.StartTangent = {1.0, 0.0, 0.0};
	SplineData.Params.EndTangent = {1.0, 0.0, 0.0};
	SplineData.Params.SourceForwardMin = 0.0;
	SplineData.Params.SourceForwardMax = 1.0;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FSplineMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1, SplineData), Translate(0.0, -1.0));
	auto Pose = std::make_shared<Durin::FSkeletalPosePalette>();
	Pose->Revision = 1;
	Pose->SkeletonCompatibilityIdentity = "GBufferQualification";
	Pose->Matrices = {Durin::FMatrix4f(1.0f)};
	Pose->LocalBounds = SkeletalQuad->LocalBounds;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(3), std::make_unique<Durin::FSkeletalMeshSceneProxy>(SkeletalQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1, Pose), Translate(-1.0, 0.0));
	const std::array<uint16, 4> Heights{};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> HeightPayload;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(
		2, 2, Heights, HeightPayload, Error
	)) << Error;
	Durin::FTerrainPatchDescriptor Patch{
		.OriginX = 0, .OriginY = 0, .CellCountX = 1, .CellCountY = 1, .LODSteps = {1}, .LODErrors = {0.0}, .LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})
	};
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(4), std::make_unique<Durin::FTerrainSceneProxy>(HeightPayload, 1, 1.0, 1.0, 0.0, 0.0, std::vector<Durin::FTerrainPatchDescriptor>{Patch}, Patch.LocalBounds, Material, 1), Durin::FMatrix(1.0));
	Durin::FDirectionalLightSceneData Directional;
	Directional.Direction = {0.35, 0.2, -1.0};
	Directional.Color = {1.0f, 1.0f, 1.0f};
	Directional.Intensity = 3.0f;
	Directional.bCastShadows = true;
	auto* DirectionalToken = PublishLightForTest<Durin::FDirectionalLightSceneProxy>(
		Scene, Durin::FLightSceneId(100), Directional);
	Durin::FPointLightSceneData PointA;
	PointA.Position = {-1.5, -0.75, 2.0};
	PointA.Color = {1.0f, 0.15f, 0.05f};
	PointA.Intensity = 5.0f;
	PointA.Range = 6.0f;
	PublishLightForTest<Durin::FPointLightSceneProxy>(
		Scene, Durin::FLightSceneId(20), PointA);
	Durin::FSpotLightSceneData SpotA;
	SpotA.Position = {1.5, -0.75, 3.0};
	SpotA.Direction = {-0.15, 0.1, -1.0};
	SpotA.Color = {0.05f, 0.2f, 1.0f};
	SpotA.Intensity = 8.0f;
	SpotA.Range = 8.0f;
	SpotA.InnerConeAngle = 25.0f;
	SpotA.OuterConeAngle = 40.0f;
	PublishLightForTest<Durin::FSpotLightSceneProxy>(
		Scene, Durin::FLightSceneId(21), SpotA);
	auto PointB = PointA;
	PointB.Position = {-0.5, 1.25, 1.5};
	PointB.Color = {0.1f, 1.0f, 0.2f};
	PointB.Intensity = 3.0f;
	PointB.Range = 4.0f;
	PublishLightForTest<Durin::FPointLightSceneProxy>(
		Scene, Durin::FLightSceneId(22), PointB);
	auto SpotB = SpotA;
	SpotB.Position = {1.25, 1.25, 2.5};
	SpotB.Direction = {0.1, -0.2, -1.0};
	SpotB.Color = {1.0f, 0.6f, 0.1f};
	SpotB.Intensity = 6.0f;
	SpotB.Range = 7.0f;
	SpotB.InnerConeAngle = 20.0f;
	SpotB.OuterConeAngle = 45.0f;
	PublishLightForTest<Durin::FSpotLightSceneProxy>(
		Scene, Durin::FLightSceneId(23), SpotB);
	Durin::FlushRenderingCommands();

	auto* SpecularAAMaterialObject = MakeMaterial(
		"GBufferQualificationSpecularAA", Durin::EMaterialBlendMode::Opaque,
		Durin::EMaterialShadingModel::Lit, {0.7, 0.4, 0.2}, 0.8f, 0.045f);
	ASSERT_NE(SpecularAAMaterialObject, nullptr);
	auto SpecularAAMaterial =
		SpecularAAMaterialObject->GetMaterialRenderProxy();
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner SpecularAASceneOwner;

	Durin::FScene& SpecularAAScene = *SpecularAASceneOwner;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(SpecularAAScene,
		Durin::FPrimitiveSceneId(200),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			SpecularAAQuad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{SpecularAAMaterial}, 1),
		Durin::FMatrix(1.0));
	Durin::FDirectionalLightSceneData SpecularAADirectional;
	SpecularAADirectional.Direction = {0.25, 0.0, -1.0};
	SpecularAADirectional.Color = {1.0f, 1.0f, 1.0f};
	SpecularAADirectional.Intensity = 6.0f;
	PublishLightForTest<Durin::FDirectionalLightSceneProxy>(
		SpecularAAScene, Durin::FLightSceneId(201), SpecularAADirectional);
	Durin::FlushRenderingCommands();
	auto CaptureSpecularAASurface = [&Renderer, &SpecularAAScene](
		bool bEnableSpecularAA, Durin::FByteArray& Pixels) {
		GSpecularAASurfacePixels = &Pixels;
		Durin::SetGBufferCaptureSink(CaptureSpecularAAGBuffer);
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[&Renderer, &SpecularAAScene, bEnableSpecularAA](
				Durin::FRHICommandListImmediate& CommandList) {
				constexpr uint32 Width = 32;
				constexpr uint32 Height = 32;
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
					"SpecularAAQualification", Width, Height,
					Durin::EPixelFormat::SRGBA8_UNORM)
					.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
						| Durin::ETextureCreateFlags::ShaderResource);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				View.ViewProjectionMatrix = Durin::FMatrix(1.0);
				View.ViewportWidth = Width;
				View.ViewportHeight = Height;
				View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
				View.Settings.Mode.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.Mode.bEnableSpecularAA = bEnableSpecularAA;
				Durin::FScopedRendererQualificationPolicy Qualification({
					.bEnableGBuffer = true});
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(
					Renderer.RenderView(
						CommandList, &SpecularAAScene, View, Target, false,
						{}),
					Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
		Durin::SetGBufferCaptureSink(nullptr);
		GSpecularAASurfacePixels = nullptr;
	};
	Durin::FByteArray SpecularAADisabledSurface;
	Durin::FByteArray SpecularAAEnabledSurface;
	CaptureSpecularAASurface(false, SpecularAADisabledSurface);
	CaptureSpecularAASurface(true, SpecularAAEnabledSurface);
	ASSERT_EQ(SpecularAADisabledSurface.size(), 32u * 32u * 4u);
	ASSERT_EQ(SpecularAAEnabledSurface.size(), SpecularAADisabledSurface.size());
	size_t ValidPixels = 0;
	size_t BroadenedPixels = 0;
	for (size_t Offset = 0; Offset < SpecularAADisabledSurface.size(); Offset += 4)
	{
		if (SpecularAADisabledSurface[Offset + 3] == std::byte{0}) continue;
		++ValidPixels;
		EXPECT_EQ(
			SpecularAAEnabledSurface[Offset + 1],
			SpecularAADisabledSurface[Offset + 1]);
		EXPECT_EQ(
			SpecularAAEnabledSurface[Offset + 2],
			SpecularAADisabledSurface[Offset + 2]);
		EXPECT_EQ(
			SpecularAAEnabledSurface[Offset + 3],
			SpecularAADisabledSurface[Offset + 3]);
		EXPECT_GE(
			std::to_integer<uint8>(SpecularAAEnabledSurface[Offset]),
			std::to_integer<uint8>(SpecularAADisabledSurface[Offset]));
		if (SpecularAAEnabledSurface[Offset]
			> SpecularAADisabledSurface[Offset]) ++BroadenedPixels;
	}
	EXPECT_GT(ValidPixels, 0u);
	EXPECT_GT(BroadenedPixels, 0u);

	// Measure the final display output so the spatial edge filter is part of the
	// matrix. A fixed center ROI excludes the moving silhouette and isolates the
	// varying-normal highlight while subpixel translations emulate camera motion.
	struct FSpecularAAStabilityResult
	{
		double MeanAdjacentDifference = 0.0;
		double MeanSignal = 0.0;
		double FrameMeanRange = 0.0;
		double FramePeakRange = 0.0;
	};
	auto MeasureSpecularAAStability = [&Renderer, &SpecularAAScene](
		bool bEnableSpecularAA, bool bEnableFXAA, double Scale) {
		constexpr uint32 Width = 192;
		constexpr uint32 Height = 192;
		constexpr std::array<double, 9> MotionPixels{
			-1.0, -0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75, 1.0};
		std::array<Durin::FByteArray, MotionPixels.size()> Frames;
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[&Renderer, &SpecularAAScene, bEnableSpecularAA, bEnableFXAA,
			 Scale, &Frames, &MotionPixels](
				Durin::FRHICommandListImmediate& CommandList) {
				for (size_t FrameIndex = 0; FrameIndex < MotionPixels.size();
					 ++FrameIndex)
				{
					const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
						"SpecularAAStability", Width, Height,
						Durin::EPixelFormat::SRGBA8_UNORM)
						.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
							| Durin::ETextureCreateFlags::ShaderResource
							| Durin::ETextureCreateFlags::SourceCopy);
					Durin::FTextureRHIRef Target =
						Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Target, nullptr);
					Durin::FSceneView View;
					const double Offset = 2.0 * MotionPixels[FrameIndex] / Width;
					View.ViewProjectionMatrix =
						Durin::Math::TranslationMatrix(
							Durin::FVector3{Offset, 0.0, 0.0})
						* Durin::Math::ScaleMatrix(
							Durin::FVector3{Scale, Scale, 1.0});
					View.ViewLocation = {0.0, 0.0, 1.0};
					View.ViewportWidth = Width;
					View.ViewportHeight = Height;
					View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
					View.Settings.Mode.VisibilityMode =
						Durin::EViewVisibilityMode::FrustumCullingDisabled;
					View.Settings.Mode.bEnableSpecularAA = bEnableSpecularAA;
					View.Settings.PostProcess.bEnableFXAA = bEnableFXAA;
					++Durin::GRenderFrameCounterRenderThread;
					Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					EXPECT_EQ(
						Renderer.RenderView(
							CommandList, &SpecularAAScene, View, Target, false,
							Durin::FSceneViewRenderOptions{}),
						Durin::ERenderViewResult::Success);
					ReadColorTexture(CommandList, Target, Frames[FrameIndex]);
					Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			});
		Durin::FlushRenderingCommands();
		{
			const std::string FileName = std::string("specular-aa-")
				+ (bEnableSpecularAA ? "enabled" : "disabled") + "-fxaa-"
				+ (bEnableFXAA ? "on" : "off") + "-scale-"
				+ std::to_string(static_cast<int>(Scale * 100.0)) + ".ppm";
			std::ofstream Capture(
				Durin::Testing::GetTestWorkDirectory() / FileName,
				std::ios::binary);
			Capture << "P6\n" << Width << ' ' << Height << "\n255\n";
			for (size_t Offset = 0; Offset < Frames[4].size(); Offset += 4)
			{
				Capture.put(static_cast<char>(Frames[4][Offset]));
				Capture.put(static_cast<char>(Frames[4][Offset + 1]));
				Capture.put(static_cast<char>(Frames[4][Offset + 2]));
			}
		}

		double AdjacentDifference = 0.0;
		double Signal = 0.0;
		size_t Samples = 0;
		std::array<double, MotionPixels.size()> FrameMeans{};
		std::array<double, MotionPixels.size()> FramePeaks{};
		const uint32 Radius = static_cast<uint32>(18.0 * Scale);
		for (size_t FrameIndex = 0; FrameIndex < Frames.size(); ++FrameIndex)
		{
			EXPECT_EQ(Frames[FrameIndex].size(), Width * Height * 4u);
			for (uint32 Y = Height / 2 - Radius;
				 Y < Height / 2 + Radius; ++Y)
			{
				for (uint32 X = Width / 2 - Radius;
					 X < Width / 2 + Radius; ++X)
				{
					const size_t Offset =
						(static_cast<size_t>(Y) * Width + X) * 4u;
					auto Luminance = [&Frames, Offset](size_t Index) {
						const auto Channel = [&Frames, Offset, Index](size_t C) {
							return std::to_integer<uint8>(
								Frames[Index][Offset + C]) / 255.0;
						};
						return 0.2126 * Channel(0) + 0.7152 * Channel(1)
							+ 0.0722 * Channel(2);
					};
					const double Current = Luminance(FrameIndex);
					Signal += Current;
					FrameMeans[FrameIndex] += Current;
					FramePeaks[FrameIndex] =
						std::max(FramePeaks[FrameIndex], Current);
					if (FrameIndex > 0)
						AdjacentDifference +=
							std::abs(Current - Luminance(FrameIndex - 1));
					++Samples;
				}
			}
		}
		const double PixelsPerFrame =
			static_cast<double>((Radius * 2u) * (Radius * 2u));
		for (double& Mean : FrameMeans) Mean /= PixelsPerFrame;
		return FSpecularAAStabilityResult{
			.MeanAdjacentDifference = AdjacentDifference
				/ static_cast<double>(Samples - Samples / Frames.size()),
			.MeanSignal = Signal / static_cast<double>(Samples),
			.FrameMeanRange = *std::ranges::max_element(FrameMeans)
				- *std::ranges::min_element(FrameMeans),
			.FramePeakRange = *std::ranges::max_element(FramePeaks)
				- *std::ranges::min_element(FramePeaks)};
	};

	for (const bool bEnableFXAA : {false, true})
	{
		for (const double Scale : {1.0, 0.875, 0.75})
		{
			const FSpecularAAStabilityResult Disabled =
				MeasureSpecularAAStability(false, bEnableFXAA, Scale);
			const FSpecularAAStabilityResult Enabled =
				MeasureSpecularAAStability(true, bEnableFXAA, Scale);
			std::cout << "Specular AA stability: fxaa=" << bEnableFXAA
				<< ",scale=" << Scale
				<< ",disabled_delta=" << Disabled.MeanAdjacentDifference
				<< ",enabled_delta=" << Enabled.MeanAdjacentDifference
				<< ",disabled_signal=" << Disabled.MeanSignal
				<< ",enabled_signal=" << Enabled.MeanSignal
				<< ",disabled_mean_range=" << Disabled.FrameMeanRange
				<< ",enabled_mean_range=" << Enabled.FrameMeanRange
				<< ",disabled_peak_range=" << Disabled.FramePeakRange
				<< ",enabled_peak_range=" << Enabled.FramePeakRange << '\n';
			EXPECT_GT(Disabled.MeanSignal, 0.01);
			EXPECT_GT(Enabled.MeanSignal, 0.01);
			// The production studio IBL remains active in this final-output matrix.
			// Require at least a 70% peak-range reduction across its combined
			// specular response and the optional FXAA pass.
			EXPECT_LT(
				Enabled.FramePeakRange,
				Disabled.FramePeakRange * 0.30);
		}
	}

	std::vector<Durin::FGPUTimingQueryRHIRef> GBufferQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> DeferredQueries;
	GGBufferTimingQueries = &GBufferQueries;
	GDeferredTimingQueries = &DeferredQueries;
	Durin::SetGBufferTimingQuerySink(CaptureGBufferTiming);
	Durin::SetDeferredDirectionalTimingQuerySink(CaptureDeferredTiming);
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "GBufferQualificationColor", TimingWidth, TimingHeight,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = TimingWidth;
			View.ViewportHeight = TimingHeight;
			// Keep the qualification target isolated from the production deferred
			// interval while exercising the same Lit material records.
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.DirectionalShadow.Candidate =
				Durin::EDirectionalShadowCandidate::SingleMap;
			View.Settings.DirectionalShadow.FilterQuality =
				Durin::EDirectionalShadowFilterQuality::Medium;
			Durin::FScopedRendererQualificationPolicy Qualification({
				.bEnableGBuffer = true,
				.bEnableDeferredDirectional = true});
			for (uint32 Frame = 0;
				 Frame < WarmupFrames + MeasuredFrames; ++Frame)
			{
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		}
	);
	Durin::FlushRenderingCommands();
	Durin::SetGBufferTimingQuerySink(nullptr);
	Durin::SetDeferredDirectionalTimingQuerySink(nullptr);
	GGBufferTimingQueries = nullptr;
	GDeferredTimingQueries = nullptr;
	for (uint32 Attempt = 0; Attempt < 100; ++Attempt)
	{
		auto AllReady = [](const auto& Queries) {
			return std::ranges::all_of(Queries, [](const auto& Query) {
				return Query->GetResult().State
					   == Durin::ERHIGPUTimingResultState::Ready;
			});
		};
		const bool bReady =
			GBufferQueries.size() == WarmupFrames + MeasuredFrames
			&& DeferredQueries.size() == WarmupFrames + MeasuredFrames
			&& AllReady(GBufferQueries) && AllReady(DeferredQueries);
		if (bReady) break;
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_EQ(GBufferQueries.size(), WarmupFrames + MeasuredFrames);
	ASSERT_EQ(DeferredQueries.size(), WarmupFrames + MeasuredFrames);
	std::vector<uint64> GBufferDurations;
	std::vector<uint64> DeferredDurations;
	std::vector<uint64> CombinedDurations;
	for (size_t Index = WarmupFrames; Index < GBufferQueries.size(); ++Index)
	{
		const Durin::FRHIGPUTimingResult GBufferResult =
			GBufferQueries[Index]->GetResult();
		const Durin::FRHIGPUTimingResult DeferredResult =
			DeferredQueries[Index]->GetResult();
		ASSERT_EQ(GBufferResult.State, Durin::ERHIGPUTimingResultState::Ready);
		ASSERT_EQ(DeferredResult.State, Durin::ERHIGPUTimingResultState::Ready);
		GBufferDurations.push_back(GBufferResult.DurationNanoseconds);
		DeferredDurations.push_back(DeferredResult.DurationNanoseconds);
		CombinedDurations.push_back(
			GBufferResult.DurationNanoseconds
			+ DeferredResult.DurationNanoseconds
		);
	}
	std::ranges::sort(GBufferDurations);
	std::ranges::sort(DeferredDurations);
	std::ranges::sort(CombinedDurations);
	ASSERT_EQ(GBufferDurations.size(), MeasuredFrames);
	auto Median = [](const std::vector<uint64>& Durations) {
		const size_t Upper = Durations.size() / 2;
		const size_t Lower = (Durations.size() - 1) / 2;
		return (Durations[Lower] + Durations[Upper]) / 2;
	};
	auto Percentile95 = [](const std::vector<uint64>& Durations) {
		return Durations[(Durations.size() * 95u + 99u) / 100u - 1u];
	};
	const uint64 GBufferMedian = Median(GBufferDurations);
	const uint64 GBufferP95 = Percentile95(GBufferDurations);
	const uint64 DeferredMedian = Median(DeferredDurations);
	const uint64 DeferredP95 = Percentile95(DeferredDurations);
	const uint64 CombinedMedian = Median(CombinedDurations);
	const uint64 CombinedP95 = Percentile95(CombinedDurations);
	if (bNamedAdapter)
	{
		EXPECT_LE(GBufferMedian, 350'000u);
		EXPECT_LE(GBufferP95, 500'000u);
		EXPECT_LE(DeferredMedian, 450'000u);
		EXPECT_LE(DeferredP95, 650'000u);
		EXPECT_LE(CombinedMedian, 800'000u);
		EXPECT_LE(CombinedP95, 1'000'000u);
	}
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferAttemptedDraws, 4u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferSuccessfulDraws, 4u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferRejectedDraws, 0u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferSkippedDraws, 0u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferStaticMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferSplineMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferSkeletalMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferTerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GLastTelemetry.Lighting.SelectedDirectionalLights, 1u);
	EXPECT_EQ(GLastTelemetry.Lighting.SelectedPointLights, 2u);
	EXPECT_EQ(GLastTelemetry.Lighting.SelectedSpotLights, 2u);
	EXPECT_EQ(GLastTelemetry.Lighting.OverflowPointLights, 0u);
	EXPECT_EQ(GLastTelemetry.Lighting.OverflowSpotLights, 0u);
	EXPECT_EQ(GLastTelemetry.Deferred.DeferredDirectionalEnabledViews, 1u);
	EXPECT_EQ(GLastTelemetry.Deferred.DeferredDirectionalUnavailableViews, 0u);
	EXPECT_EQ(GLastTelemetry.Deferred.DeferredDirectionalPassFailures, 0u);
	EXPECT_EQ(GLastTelemetry.Deferred.DeferredDirectionalOutputBytes, Durin::FDeferredDirectionalLightingRenderer::CalculateTargetBytes(TimingWidth, TimingHeight));
	const uint64 AttachmentBytes =
		Durin::FGBufferRenderer::CalculateTargetBytes(TimingWidth, TimingHeight);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferAttachmentBytes, AttachmentBytes);
	EXPECT_LE(AttachmentBytes, Durin::FGBufferRenderer::MaximumRetainedBytes);
	std::cout << "Deferred lighting qualification: status="
			  << (bNamedAdapter ? "named_gate" : "observation")
			  << ", adapter=" << DeviceName
			  << ", vulkan=" << vk::apiVersionMajor(DeviceProperties.apiVersion)
			  << '.' << vk::apiVersionMinor(DeviceProperties.apiVersion)
			  << '.' << vk::apiVersionPatch(DeviceProperties.apiVersion)
			  << ", driver=" << DeviceProperties.driverVersion << ", "
			  << "resolution=1920x1080, warmup=" << WarmupFrames
			  << ", samples=" << MeasuredFrames
			  << ", gbuffer_median_ns=" << GBufferMedian
			  << ", gbuffer_p95_ns=" << GBufferP95
			  << ", deferred_median_ns=" << DeferredMedian
			  << ", deferred_p95_ns=" << DeferredP95
			  << ", combined_median_ns=" << CombinedMedian
			  << ", combined_p95_ns=" << CombinedP95
			  << ", gbuffer_bytes=" << AttachmentBytes
			  << ", deferred_bytes="
			  << GLastTelemetry.Deferred.DeferredDirectionalOutputBytes
			  << ", active_route_bytes="
			  << 107'827'200u << "\n";
	GBufferQueries.clear();
	DeferredQueries.clear();
	std::vector<Durin::FGPUTimingQueryRHIRef> AmbientOcclusionQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> AmbientOcclusionFilterQueries;
	GGroundTruthAmbientOcclusionTimingQueries = &AmbientOcclusionQueries;
	GGroundTruthAmbientOcclusionFilterTimingQueries =
		&AmbientOcclusionFilterQueries;
	Durin::SetGroundTruthAmbientOcclusionTimingQuerySink(
		CaptureGroundTruthAmbientOcclusionTiming
	);
	Durin::SetGroundTruthAmbientOcclusionFilterTimingQuerySink(
		CaptureGroundTruthAmbientOcclusionFilterTiming
	);
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "GroundTruthAmbientOcclusionQualification", TimingWidth, TimingHeight,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = TimingWidth;
			View.ViewportHeight = TimingHeight;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.AmbientOcclusion.Quality =
				Durin::EGroundTruthAmbientOcclusionQuality::FullResolution;
			Durin::FScopedRendererQualificationPolicy Qualification({
				.bEnableGroundTruthAmbientOcclusion = true});
			for (uint32 Frame = 0;
				 Frame < WarmupFrames + MeasuredFrames; ++Frame)
			{
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		}
	);
	Durin::FlushRenderingCommands();
	Durin::SetGroundTruthAmbientOcclusionTimingQuerySink(nullptr);
	Durin::SetGroundTruthAmbientOcclusionFilterTimingQuerySink(nullptr);
	GGroundTruthAmbientOcclusionTimingQueries = nullptr;
	GGroundTruthAmbientOcclusionFilterTimingQueries = nullptr;
	for (uint32 Attempt = 0; Attempt < 100; ++Attempt)
	{
		const bool bReady =
			AmbientOcclusionQueries.size() == WarmupFrames + MeasuredFrames
			&& AmbientOcclusionFilterQueries.size()
				   == WarmupFrames + MeasuredFrames
			&& std::ranges::all_of(AmbientOcclusionQueries, [](const auto& Query) {
				   return Query->GetResult().State
						  == Durin::ERHIGPUTimingResultState::Ready;
			   })
			&& std::ranges::all_of(AmbientOcclusionFilterQueries, [](const auto& Query) {
				   return Query->GetResult().State
						  == Durin::ERHIGPUTimingResultState::Ready;
			   });
		if (bReady) break;
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_EQ(AmbientOcclusionQueries.size(), WarmupFrames + MeasuredFrames);
	ASSERT_EQ(AmbientOcclusionFilterQueries.size(), WarmupFrames + MeasuredFrames);
	std::vector<uint64> AmbientOcclusionDurations;
	std::vector<uint64> AmbientOcclusionFilterDurations;
	std::vector<uint64> AmbientOcclusionCombinedDurations;
	for (size_t Index = WarmupFrames; Index < AmbientOcclusionQueries.size(); ++Index)
	{
		const auto Result = AmbientOcclusionQueries[Index]->GetResult();
		ASSERT_EQ(Result.State, Durin::ERHIGPUTimingResultState::Ready);
		AmbientOcclusionDurations.push_back(Result.DurationNanoseconds);
		const auto FilterResult =
			AmbientOcclusionFilterQueries[Index]->GetResult();
		ASSERT_EQ(FilterResult.State, Durin::ERHIGPUTimingResultState::Ready);
		AmbientOcclusionFilterDurations.push_back(
			FilterResult.DurationNanoseconds
		);
		AmbientOcclusionCombinedDurations.push_back(
			Result.DurationNanoseconds + FilterResult.DurationNanoseconds
		);
	}
	std::ranges::sort(AmbientOcclusionDurations);
	std::ranges::sort(AmbientOcclusionFilterDurations);
	std::ranges::sort(AmbientOcclusionCombinedDurations);
	const uint64 AmbientOcclusionMedian = Median(AmbientOcclusionDurations);
	const uint64 AmbientOcclusionP95 =
		Percentile95(AmbientOcclusionDurations);
	const uint64 AmbientOcclusionFilterMedian =
		Median(AmbientOcclusionFilterDurations);
	const uint64 AmbientOcclusionFilterP95 =
		Percentile95(AmbientOcclusionFilterDurations);
	const uint64 AmbientOcclusionCombinedMedian =
		Median(AmbientOcclusionCombinedDurations);
	const uint64 AmbientOcclusionCombinedP95 =
		Percentile95(AmbientOcclusionCombinedDurations);
	EXPECT_GT(AmbientOcclusionMedian, 0u);
	EXPECT_GT(AmbientOcclusionFilterMedian, 0u);
	if (bNamedAdapter)
	{
		EXPECT_LE(AmbientOcclusionMedian, 600'000u);
		EXPECT_LE(AmbientOcclusionFilterMedian, 250'000u);
		EXPECT_LE(AmbientOcclusionCombinedMedian, 850'000u);
	}
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionAttemptedViews, 1u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionEnabledViews, 1u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionUnavailableViews, 0u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionRawPassFailures, 0u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionFilterPassFailures, 0u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionResolvePassFailures, 0u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionFullResolutionViews, 1u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes, 4'147'200u);
	std::cout << "GTAO_RAW_QUALIFICATION status="
			  << (bNamedAdapter ? "named_gate" : "observation")
			  << ",gpu=\"" << DeviceName << "\""
			  << ",driver=" << DeviceProperties.driverVersion
			  << ",vulkan=" << vk::apiVersionMajor(DeviceProperties.apiVersion)
			  << '.' << vk::apiVersionMinor(DeviceProperties.apiVersion)
			  << '.' << vk::apiVersionPatch(DeviceProperties.apiVersion)
			  << ",configuration=Win64-Debug-DurinEditor,validation=enabled"
			  << ",resolution=1920x1080,warmup_frames=" << WarmupFrames
			  << ",measured_frames=" << MeasuredFrames
			  << ",median_ns=" << AmbientOcclusionMedian
			  << ",p95_ns=" << AmbientOcclusionP95
			  << ",filter_median_ns=" << AmbientOcclusionFilterMedian
			  << ",filter_p95_ns=" << AmbientOcclusionFilterP95
			  << ",combined_median_ns=" << AmbientOcclusionCombinedMedian
			  << ",combined_p95_ns=" << AmbientOcclusionCombinedP95
			  << ",active_bytes="
			  << GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes << '\n';
	AmbientOcclusionQueries.clear();
	AmbientOcclusionFilterQueries.clear();

	constexpr uint32 CaptureWidth = 320;
	constexpr uint32 CaptureHeight = 180;
	auto CaptureAmbientOcclusionFrame = [&Renderer, &Scene](
											uint32 Width, uint32 Height,
											Durin::FByteArray& Pixels,
											Durin::FByteArray& FilteredPixels
										) {
		GGroundTruthAmbientOcclusionPixels = &Pixels;
		GGroundTruthAmbientOcclusionFilteredPixels = &FilteredPixels;
		Durin::SetGroundTruthAmbientOcclusionCaptureSink(
			CaptureGroundTruthAmbientOcclusion
		);
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[&Renderer, &Scene, Width, Height](
				Durin::FRHICommandListImmediate& CommandList
			) {
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
									  "GroundTruthAmbientOcclusionCapture", Width, Height,
									  Durin::EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				// Look down world -Z using Durin's X-forward view convention.
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
				View.ViewProjectionMatrix =
					View.ProjectionMatrix * View.ViewMatrix;
				View.ViewportWidth = Width;
				View.ViewportHeight = Height;
				View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
				View.Settings.Mode.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.AmbientOcclusion.Quality =
					Durin::EGroundTruthAmbientOcclusionQuality::FullResolution;
				Durin::FScopedRendererQualificationPolicy Qualification({
					.bEnableGroundTruthAmbientOcclusion = true});
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		Durin::SetGroundTruthAmbientOcclusionCaptureSink(nullptr);
		GGroundTruthAmbientOcclusionPixels = nullptr;
		GGroundTruthAmbientOcclusionFilteredPixels = nullptr;
	};
	Durin::FByteArray FirstRawVisibility;
	Durin::FByteArray FirstFilteredVisibility;
	Durin::FByteArray ResizedRawVisibility;
	Durin::FByteArray ResizedFilteredVisibility;
	Durin::FByteArray RepeatedRawVisibility;
	Durin::FByteArray RepeatedFilteredVisibility;
	CaptureAmbientOcclusionFrame(
		CaptureWidth, CaptureHeight,
		FirstRawVisibility, FirstFilteredVisibility
	);
	CaptureAmbientOcclusionFrame(
		384, 216, ResizedRawVisibility, ResizedFilteredVisibility
	);
	CaptureAmbientOcclusionFrame(
		CaptureWidth, CaptureHeight,
		RepeatedRawVisibility, RepeatedFilteredVisibility
	);
	ASSERT_EQ(FirstRawVisibility.size(), CaptureWidth * CaptureHeight);
	ASSERT_EQ(FirstFilteredVisibility.size(), CaptureWidth * CaptureHeight);
	ASSERT_EQ(ResizedRawVisibility.size(), 384u * 216u);
	ASSERT_EQ(ResizedFilteredVisibility.size(), 384u * 216u);
	EXPECT_EQ(RepeatedRawVisibility, FirstRawVisibility);
	EXPECT_EQ(RepeatedFilteredVisibility, FirstFilteredVisibility);
	auto PixelAt = [&FirstRawVisibility](uint32 X, uint32 Y) {
		return std::to_integer<uint32>(
			FirstRawVisibility[Y * CaptureWidth + X]);
	};
	EXPECT_GE(PixelAt(80, 45), 250u);
	EXPECT_GE(PixelAt(240, 45), 250u);
	EXPECT_GE(PixelAt(80, 135), 250u);
	EXPECT_GE(PixelAt(240, 135), 250u);
	auto FilteredPixelAt = [&FirstFilteredVisibility](
							   uint32 X, uint32 Y
						   ) {
		return std::to_integer<uint32>(
			FirstFilteredVisibility[Y * CaptureWidth + X]);
	};
	EXPECT_GE(FilteredPixelAt(80, 45), 250u);
	EXPECT_GE(FilteredPixelAt(240, 45), 250u);
	EXPECT_GE(FilteredPixelAt(80, 135), 250u);
	EXPECT_GE(FilteredPixelAt(240, 135), 250u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes, 2u * CaptureWidth * CaptureHeight);

	Durin::FMatrix RaisedTransform = Durin::Math::TranslationMatrix(
		Durin::FVector3{-0.25, -0.25, 0.25}
	);
	RaisedTransform = Durin::Math::Scale(
		RaisedTransform, Durin::FVector3{0.5, 0.5, 1.0});
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(7), std::make_unique<Durin::FStaticMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1), RaisedTransform);
	Durin::FlushRenderingCommands();
	Durin::FByteArray RaisedContactVisibility;
	Durin::FByteArray RaisedContactFilteredVisibility;
	CaptureAmbientOcclusionFrame(
		CaptureWidth, CaptureHeight,
		RaisedContactVisibility, RaisedContactFilteredVisibility
	);
	ASSERT_EQ(RaisedContactVisibility.size(), FirstRawVisibility.size());
	ASSERT_EQ(RaisedContactFilteredVisibility.size(), FirstFilteredVisibility.size());
	size_t OccludedPixels = 0;
	for (size_t Index = 0; Index < RaisedContactVisibility.size(); ++Index)
	{
		if (std::to_integer<uint32>(RaisedContactVisibility[Index]) + 2u
			< std::to_integer<uint32>(FirstRawVisibility[Index]))
			++OccludedPixels;
	}
	EXPECT_GT(OccludedPixels, 0u);
	EXPECT_LT(OccludedPixels, CaptureWidth * CaptureHeight);
	size_t FilteredOccludedPixels = 0;
	for (size_t Index = 0; Index < RaisedContactFilteredVisibility.size(); ++Index)
	{
		if (std::to_integer<uint32>(RaisedContactFilteredVisibility[Index])
				+ 2u
			< std::to_integer<uint32>(FirstFilteredVisibility[Index]))
			++FilteredOccludedPixels;
	}
	EXPECT_GT(FilteredOccludedPixels, 0u);
	EXPECT_LT(FilteredOccludedPixels, CaptureWidth * CaptureHeight);
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(7));
	Durin::FlushRenderingCommands();

	auto* UnlitMaterialObject = MakeMaterial(
		"GBufferQualificationUnlit", Durin::EMaterialBlendMode::Opaque,
		Durin::EMaterialShadingModel::Unlit, {0.1, 0.8, 0.3});
	ASSERT_NE(UnlitMaterialObject, nullptr);
	auto UnlitMaterial = UnlitMaterialObject->GetMaterialRenderProxy();
	auto* TranslucentMaterialObject = MakeMaterial(
		"GBufferQualificationTranslucent",
		Durin::EMaterialBlendMode::Translucent,
		Durin::EMaterialShadingModel::Unlit, {0.8, 0.2, 0.5}, 0.0f, 0.5f,
		0.45f);
	ASSERT_NE(TranslucentMaterialObject, nullptr);
	auto TranslucentMaterial =
		TranslucentMaterialObject->GetMaterialRenderProxy();
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(5), std::make_unique<Durin::FStaticMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{UnlitMaterial}, 1), Translate(-0.55, -0.45));
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(6), std::make_unique<Durin::FStaticMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{TranslucentMaterial}, 1), Translate(-0.35, -0.25));
	Durin::FlushRenderingCommands();

	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionGBufferQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionSceneQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionDeferredQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionRetainedOpaqueQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionVolumetricCloudQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef>
		ProductionSortedTranslucencyQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionPostProcessQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionShadowQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionContactQueries;
	std::vector<uint64> ProductionGBufferDurations;
	std::vector<uint64> ProductionSceneDurations;
	std::vector<uint64> ProductionDeferredDurations;
	std::vector<uint64> ProductionDeferredWithoutAODurations;
	std::vector<uint64> ProductionRetainedOpaqueDurations;
	std::vector<uint64> ProductionVolumetricCloudDurations;
	std::vector<uint64> ProductionSortedTranslucencyDurations;
	std::vector<uint64> ProductionRetainedDurations;
	std::vector<uint64> ProductionPostProcessDurations;
	std::vector<uint64> ProductionShadowDurations;
	std::vector<uint64> ProductionComputeContactDurations;
	std::vector<uint64> ProductionFragmentContactDurations;
	std::vector<uint64> ConstrainedComputeContactDurations;
	std::vector<uint64> ConstrainedFragmentContactDurations;
	bool bEnableProductionAmbientOcclusion = true;
	bool bEnableProductionContactShadows = false;
	bool bForceProductionFragmentContact = false;
	uint32 ProductionWidth = TimingWidth;
	uint32 ProductionHeight = TimingHeight;
	uint32 ProductionViewportX = 0;
	uint32 ProductionViewportY = 0;
	uint32 ProductionViewportWidth = TimingWidth;
	uint32 ProductionViewportHeight = TimingHeight;
	Durin::EGroundTruthAmbientOcclusionQuality ProductionAmbientOcclusionQuality =
		Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution;
	auto RenderProductionFrames = [&Renderer, &Scene,
								   &bEnableProductionAmbientOcclusion,
								   &bEnableProductionContactShadows,
								   &bForceProductionFragmentContact,
								   &ProductionWidth, &ProductionHeight,
								   &ProductionViewportX, &ProductionViewportY,
								   &ProductionViewportWidth,
								   &ProductionViewportHeight,
								   &ProductionAmbientOcclusionQuality](
			uint32 FrameCount) {
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[&Renderer, &Scene, bEnableProductionAmbientOcclusion,
			 bEnableProductionContactShadows, bForceProductionFragmentContact,
			 ProductionWidth, ProductionHeight, ProductionViewportX,
				 ProductionViewportY, ProductionViewportWidth,
				 ProductionViewportHeight,
				 ProductionAmbientOcclusionQuality, FrameCount](
				Durin::FRHICommandListImmediate& CommandList
			) {
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
									  "HybridProductionQualification", ProductionWidth, ProductionHeight,
									  Durin::EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				View.ViewProjectionMatrix = Durin::FMatrix(1.0);
				View.ViewportX = ProductionViewportX;
				View.ViewportY = ProductionViewportY;
				View.ViewportWidth = ProductionViewportWidth;
				View.ViewportHeight = ProductionViewportHeight;
				View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
				View.Settings.Mode.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.DirectionalShadow.Candidate =
					Durin::EDirectionalShadowCandidate::SingleMap;
				View.Settings.DirectionalShadow.FilterQuality =
					Durin::EDirectionalShadowFilterQuality::Medium;
				View.Settings.PostProcess.bEnableFXAA = true;
				View.Settings.AmbientOcclusion.bEnabled =
					bEnableProductionAmbientOcclusion;
				View.Settings.DirectionalShadow.bEnableContactShadows =
					bEnableProductionContactShadows;
				View.Settings.AmbientOcclusion.Quality =
					ProductionAmbientOcclusionQuality;
				Durin::FScopedRendererQualificationPolicy Qualification({
					.bForceFragmentContactVisibility =
						bForceProductionFragmentContact});
				for (uint32 Frame = 0; Frame < FrameCount; ++Frame)
				{
					++Durin::GRenderFrameCounterRenderThread;
					Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}), Durin::ERenderViewResult::Success);
					Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			}
		);
		Durin::FlushRenderingCommands();
	};
	auto WaitForProductionQueries = [](const auto& Queries,
			uint32 ExpectedCount) {
		for (uint32 Attempt = 0; Attempt < 100; ++Attempt)
		{
			const bool bReady = Queries.size() == ExpectedCount
								&& std::ranges::all_of(Queries, [](const auto& Query) {
									   return Query->GetResult().State
											  == Durin::ERHIGPUTimingResultState::Ready;
								   });
			if (bReady) break;
			Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
				[](Durin::FRHICommandListImmediate& CommandList) {
					++Durin::GRenderFrameCounterRenderThread;
					Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			);
			Durin::FlushRenderingCommands();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	};
	auto CollectProductionDurations = [&WaitForProductionQueries](
			auto& Queries, auto& Durations, uint32 WarmupCount,
			uint32 MeasuredCount) {
		const uint32 ExpectedCount = WarmupCount + MeasuredCount;
		WaitForProductionQueries(Queries, ExpectedCount);
		EXPECT_EQ(Queries.size(), ExpectedCount);
		if (Queries.size() == ExpectedCount)
		{
			for (size_t Index = WarmupCount; Index < Queries.size(); ++Index)
			{
				const auto Result = Queries[Index]->GetResult();
				EXPECT_EQ(Result.State,
					Durin::ERHIGPUTimingResultState::Ready);
				Durations.push_back(Result.DurationNanoseconds);
			}
		}
		Queries.clear();
	};
	auto ProfileProductionInterval = [&RenderProductionFrames,
									  &CollectProductionDurations](
			auto& Queries, auto& Durations, auto& QuerySlot,
			auto SetSink, auto CaptureSink) {
		QuerySlot = &Queries;
		SetSink(CaptureSink);
		RenderProductionFrames(WarmupFrames + MeasuredFrames);
		SetSink(nullptr);
		QuerySlot = nullptr;
		CollectProductionDurations(
			Queries, Durations, WarmupFrames, MeasuredFrames);
	};
	auto ProfileProductionRoute = [&RenderProductionFrames,
								 &CollectProductionDurations,
								 &ProductionGBufferQueries,
								 &ProductionGBufferDurations,
								 &ProductionSceneQueries,
								 &ProductionSceneDurations,
								 &ProductionDeferredQueries,
								 &ProductionDeferredDurations,
								 &ProductionRetainedOpaqueQueries,
								 &ProductionRetainedOpaqueDurations,
								 &ProductionVolumetricCloudQueries,
								 &ProductionVolumetricCloudDurations,
								 &ProductionSortedTranslucencyQueries,
								 &ProductionSortedTranslucencyDurations,
								 &ProductionPostProcessQueries,
								 &ProductionPostProcessDurations,
								 &ProductionShadowQueries,
								 &ProductionShadowDurations]() {
		GGBufferTimingQueries = &ProductionGBufferQueries;
		GSceneTimingQueries = &ProductionSceneQueries;
		GDeferredTimingQueries = &ProductionDeferredQueries;
		GRetainedOpaqueTimingQueries = &ProductionRetainedOpaqueQueries;
		GVolumetricCloudTimingQueries = &ProductionVolumetricCloudQueries;
		GSortedTranslucencyTimingQueries =
			&ProductionSortedTranslucencyQueries;
		GPostProcessTimingQueries = &ProductionPostProcessQueries;
		GShadowTimingQueries = &ProductionShadowQueries;
		Durin::SetGBufferTimingQuerySink(CaptureGBufferTiming);
		Durin::SetSceneColorTimingQuerySink(CaptureSceneTiming);
		Durin::SetDeferredDirectionalTimingQuerySink(CaptureDeferredTiming);
		Durin::SetRetainedOpaqueTimingQuerySink(CaptureRetainedOpaqueTiming);
		Durin::SetVolumetricCloudTimingQuerySink(CaptureVolumetricCloudTiming);
		Durin::SetSortedTranslucencyTimingQuerySink(
			CaptureSortedTranslucencyTiming);
		Durin::SetPostProcessTimingQuerySink(CapturePostProcessTiming);
		Durin::SetShadowDepthTimingQuerySink(CaptureShadowTiming);
		RenderProductionFrames(WarmupFrames + MeasuredFrames);
		Durin::SetShadowDepthTimingQuerySink(nullptr);
		Durin::SetPostProcessTimingQuerySink(nullptr);
		Durin::SetSortedTranslucencyTimingQuerySink(nullptr);
		Durin::SetVolumetricCloudTimingQuerySink(nullptr);
		Durin::SetRetainedOpaqueTimingQuerySink(nullptr);
		Durin::SetDeferredDirectionalTimingQuerySink(nullptr);
		Durin::SetSceneColorTimingQuerySink(nullptr);
		Durin::SetGBufferTimingQuerySink(nullptr);
		GShadowTimingQueries = nullptr;
		GPostProcessTimingQueries = nullptr;
		GSortedTranslucencyTimingQueries = nullptr;
		GVolumetricCloudTimingQueries = nullptr;
		GRetainedOpaqueTimingQueries = nullptr;
		GDeferredTimingQueries = nullptr;
		GSceneTimingQueries = nullptr;
		GGBufferTimingQueries = nullptr;
		CollectProductionDurations(
			ProductionGBufferQueries, ProductionGBufferDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionSceneQueries, ProductionSceneDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionDeferredQueries, ProductionDeferredDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionRetainedOpaqueQueries,
			ProductionRetainedOpaqueDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionVolumetricCloudQueries,
			ProductionVolumetricCloudDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionSortedTranslucencyQueries,
			ProductionSortedTranslucencyDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionPostProcessQueries, ProductionPostProcessDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionShadowQueries, ProductionShadowDurations,
			WarmupFrames, MeasuredFrames);
	};

	// Capture nested production intervals from the same frames before the
	// feature-isolation sweeps change GPU clock and thermal state.
	ProfileProductionRoute();
	const Durin::FViewRenderTelemetry ProductionRouteTelemetry = GLastTelemetry;
	bEnableProductionAmbientOcclusion = false;
	ProfileProductionInterval(
		ProductionDeferredQueries, ProductionDeferredWithoutAODurations,
		GDeferredTimingQueries,
		Durin::SetDeferredDirectionalTimingQuerySink,
		CaptureDeferredTiming);
	bEnableProductionAmbientOcclusion = true;
	std::vector<Durin::FGPUTimingQueryRHIRef> GTAOFeatureQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> GTAOResolveQueries;
	std::vector<uint64> FullGTAOFeatureDurations;
	std::vector<uint64> HalfGTAOFeatureDurations;
	std::vector<uint64> HalfGTAOResolveDurations;
	ProductionAmbientOcclusionQuality =
		Durin::EGroundTruthAmbientOcclusionQuality::FullResolution;
	ProfileProductionInterval(GTAOFeatureQueries, FullGTAOFeatureDurations,
		GGroundTruthAmbientOcclusionFeatureTimingQueries,
		Durin::SetGroundTruthAmbientOcclusionFeatureTimingQuerySink,
		CaptureGroundTruthAmbientOcclusionFeatureTiming);
	ProductionAmbientOcclusionQuality =
		Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution;
	GGroundTruthAmbientOcclusionFeatureTimingQueries = &GTAOFeatureQueries;
	GGroundTruthAmbientOcclusionResolveTimingQueries = &GTAOResolveQueries;
	Durin::SetGroundTruthAmbientOcclusionFeatureTimingQuerySink(
		CaptureGroundTruthAmbientOcclusionFeatureTiming);
	Durin::SetGroundTruthAmbientOcclusionResolveTimingQuerySink(
		CaptureGroundTruthAmbientOcclusionResolveTiming);
	RenderProductionFrames(WarmupFrames + MeasuredFrames);
	Durin::SetGroundTruthAmbientOcclusionResolveTimingQuerySink(nullptr);
	Durin::SetGroundTruthAmbientOcclusionFeatureTimingQuerySink(nullptr);
	GGroundTruthAmbientOcclusionResolveTimingQueries = nullptr;
	GGroundTruthAmbientOcclusionFeatureTimingQueries = nullptr;
	CollectProductionDurations(GTAOFeatureQueries,
		HalfGTAOFeatureDurations, WarmupFrames, MeasuredFrames);
	CollectProductionDurations(GTAOResolveQueries,
		HalfGTAOResolveDurations, WarmupFrames, MeasuredFrames);
	bEnableProductionContactShadows = true;
	bForceProductionFragmentContact = false;
	GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::Compute;
	ProfileProductionInterval(ProductionContactQueries,
		ProductionComputeContactDurations, GContactTimingQueries,
		Durin::FContactShadowVisibilityRenderer::SetTimingQuerySink,
		CaptureContactTiming);
	const Durin::FViewRenderTelemetry ProductionComputeContactTelemetry =
		GLastTelemetry;
	bForceProductionFragmentContact = true;
	GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::Fragment;
	ProfileProductionInterval(ProductionContactQueries,
		ProductionFragmentContactDurations, GContactTimingQueries,
		Durin::FContactShadowVisibilityRenderer::SetTimingQuerySink,
		CaptureContactTiming);
	const Durin::FViewRenderTelemetry ProductionFragmentContactTelemetry =
		GLastTelemetry;
	ProductionWidth = 1919;
	ProductionHeight = 1079;
	ProductionViewportX = 137;
	ProductionViewportY = 89;
	ProductionViewportWidth = 1601;
	ProductionViewportHeight = 901;
	bForceProductionFragmentContact = false;
	GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::Compute;
	ProfileProductionInterval(ProductionContactQueries,
		ConstrainedComputeContactDurations, GContactTimingQueries,
		Durin::FContactShadowVisibilityRenderer::SetTimingQuerySink,
		CaptureContactTiming);
	const Durin::FViewRenderTelemetry ConstrainedComputeContactTelemetry =
		GLastTelemetry;
	bForceProductionFragmentContact = true;
	GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::Fragment;
	ProfileProductionInterval(ProductionContactQueries,
		ConstrainedFragmentContactDurations, GContactTimingQueries,
		Durin::FContactShadowVisibilityRenderer::SetTimingQuerySink,
		CaptureContactTiming);
	const Durin::FViewRenderTelemetry ConstrainedFragmentContactTelemetry =
		GLastTelemetry;
	ProductionWidth = TimingWidth;
	ProductionHeight = TimingHeight;
	ProductionViewportX = 0;
	ProductionViewportY = 0;
	ProductionViewportWidth = TimingWidth;
	ProductionViewportHeight = TimingHeight;
	bEnableProductionContactShadows = false;
	bForceProductionFragmentContact = false;
	std::vector<uint64> ProductionTotalDurations;
	ASSERT_EQ(
		ProductionGBufferDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionSceneDurations.size(), MeasuredFrames);
	ASSERT_EQ(
		ProductionDeferredDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionDeferredWithoutAODurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionRetainedOpaqueDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionVolumetricCloudDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionSortedTranslucencyDurations.size(), MeasuredFrames);
	ASSERT_EQ(
		ProductionPostProcessDurations.size(), MeasuredFrames);
	ASSERT_EQ(
		ProductionShadowDurations.size(), MeasuredFrames);
	ASSERT_EQ(FullGTAOFeatureDurations.size(), MeasuredFrames);
	ASSERT_EQ(HalfGTAOFeatureDurations.size(), MeasuredFrames);
	ASSERT_EQ(HalfGTAOResolveDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionComputeContactDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionFragmentContactDurations.size(), MeasuredFrames);
	ASSERT_EQ(ConstrainedComputeContactDurations.size(), MeasuredFrames);
	ASSERT_EQ(ConstrainedFragmentContactDurations.size(), MeasuredFrames);
	ProductionRetainedDurations.reserve(MeasuredFrames);
	for (size_t Index = 0; Index < MeasuredFrames; ++Index)
	{
		ProductionRetainedDurations.push_back(
			ProductionRetainedOpaqueDurations[Index]
			+ ProductionVolumetricCloudDurations[Index]
			+ ProductionSortedTranslucencyDurations[Index]);
		ProductionTotalDurations.push_back(
			ProductionShadowDurations[Index]
			+ ProductionGBufferDurations[Index]
			+ ProductionSceneDurations[Index]
			+ ProductionPostProcessDurations[Index]
		);
	}
	std::ranges::sort(ProductionGBufferDurations);
	std::ranges::sort(ProductionSceneDurations);
	std::ranges::sort(ProductionDeferredDurations);
	std::ranges::sort(ProductionDeferredWithoutAODurations);
	std::ranges::sort(ProductionRetainedOpaqueDurations);
	std::ranges::sort(ProductionVolumetricCloudDurations);
	std::ranges::sort(ProductionSortedTranslucencyDurations);
	std::ranges::sort(ProductionRetainedDurations);
	std::ranges::sort(ProductionPostProcessDurations);
	std::ranges::sort(ProductionShadowDurations);
	std::ranges::sort(ProductionTotalDurations);
	std::ranges::sort(FullGTAOFeatureDurations);
	std::ranges::sort(HalfGTAOFeatureDurations);
	std::ranges::sort(HalfGTAOResolveDurations);
	std::ranges::sort(ProductionComputeContactDurations);
	std::ranges::sort(ProductionFragmentContactDurations);
	std::ranges::sort(ConstrainedComputeContactDurations);
	std::ranges::sort(ConstrainedFragmentContactDurations);
	const uint64 ProductionGBufferMedian = Median(ProductionGBufferDurations);
	const uint64 ProductionSceneMedian = Median(ProductionSceneDurations);
	const uint64 ProductionDeferredMedian = Median(ProductionDeferredDurations);
	const uint64 ProductionDeferredWithoutAOMedian =
		Median(ProductionDeferredWithoutAODurations);
	const uint64 ProductionAmbientOcclusionCompositionIncrement =
		ProductionDeferredMedian > ProductionDeferredWithoutAOMedian ? ProductionDeferredMedian - ProductionDeferredWithoutAOMedian : 0u;
	const uint64 ProductionRetainedOpaqueMedian =
		Median(ProductionRetainedOpaqueDurations);
	const uint64 ProductionVolumetricCloudMedian =
		Median(ProductionVolumetricCloudDurations);
	const uint64 ProductionSortedTranslucencyMedian =
		Median(ProductionSortedTranslucencyDurations);
	const uint64 ProductionRetainedMedian = Median(ProductionRetainedDurations);
	// These intervals are nested in the same production frames. Compare their
	// synchronized medians so isolated driver timestamp outliers cannot turn a
	// valid parent interval into a false per-sample containment failure.
	EXPECT_GE(ProductionSceneMedian,
		ProductionDeferredMedian + ProductionRetainedMedian);
	const uint64 ProductionPostProcessMedian = Median(ProductionPostProcessDurations);
	const uint64 ProductionShadowMedian = Median(ProductionShadowDurations);
	const uint64 ProductionTotalMedian = Median(ProductionTotalDurations);
	const uint64 FullGTAOFeatureMedian =
		Median(FullGTAOFeatureDurations);
	const uint64 HalfGTAOFeatureMedian =
		Median(HalfGTAOFeatureDurations);
	const uint64 HalfGTAOResolveMedian =
		Median(HalfGTAOResolveDurations);
	const uint64 ProductionComputeContactMedian =
		Median(ProductionComputeContactDurations);
	const uint64 ProductionFragmentContactMedian =
		Median(ProductionFragmentContactDurations);
	const uint64 ConstrainedComputeContactMedian =
		Median(ConstrainedComputeContactDurations);
	const uint64 ConstrainedFragmentContactMedian =
		Median(ConstrainedFragmentContactDurations);
	const uint64 ProductionGBufferP95 =
		Percentile95(ProductionGBufferDurations);
	const uint64 ProductionSceneP95 =
		Percentile95(ProductionSceneDurations);
	const uint64 ProductionDeferredP95 =
		Percentile95(ProductionDeferredDurations);
	const uint64 ProductionRetainedOpaqueP95 =
		Percentile95(ProductionRetainedOpaqueDurations);
	const uint64 ProductionVolumetricCloudP95 =
		Percentile95(ProductionVolumetricCloudDurations);
	const uint64 ProductionSortedTranslucencyP95 =
		Percentile95(ProductionSortedTranslucencyDurations);
	const uint64 ProductionRetainedP95 =
		Percentile95(ProductionRetainedDurations);
	const uint64 ProductionPostProcessP95 =
		Percentile95(ProductionPostProcessDurations);
	const uint64 ProductionShadowP95 =
		Percentile95(ProductionShadowDurations);
	const uint64 ProductionTotalP95 =
		Percentile95(ProductionTotalDurations);
	const uint64 FullGTAOFeatureP95 =
		Percentile95(FullGTAOFeatureDurations);
	const uint64 HalfGTAOFeatureP95 =
		Percentile95(HalfGTAOFeatureDurations);
	const uint64 HalfGTAOResolveP95 =
		Percentile95(HalfGTAOResolveDurations);
	const uint64 ProductionComputeContactP95 =
		Percentile95(ProductionComputeContactDurations);
	const uint64 ProductionFragmentContactP95 =
		Percentile95(ProductionFragmentContactDurations);
	const uint64 ConstrainedComputeContactP95 =
		Percentile95(ConstrainedComputeContactDurations);
	const uint64 ConstrainedFragmentContactP95 =
		Percentile95(ConstrainedFragmentContactDurations);
	EXPECT_GT(ProductionComputeContactMedian, 0u);
	EXPECT_GT(ProductionFragmentContactMedian, 0u);
	EXPECT_LE(ProductionComputeContactMedian,
		ProductionFragmentContactMedian + 300'000u);
	EXPECT_GT(ConstrainedComputeContactMedian, 0u);
	EXPECT_GT(ConstrainedFragmentContactMedian, 0u);
	EXPECT_LE(ConstrainedComputeContactMedian,
		ConstrainedFragmentContactMedian + 300'000u);
	EXPECT_EQ(ProductionComputeContactTelemetry.ContactShadow.ContactShadowDispatches, 1u);
	EXPECT_EQ(ProductionComputeContactTelemetry.ContactShadow.ContactShadowDraws, 0u);
	EXPECT_EQ(ProductionFragmentContactTelemetry.ContactShadow.ContactShadowDispatches, 0u);
	EXPECT_EQ(ProductionFragmentContactTelemetry.ContactShadow.ContactShadowDraws, 1u);
	EXPECT_EQ(ConstrainedComputeContactTelemetry.ContactShadow.ContactShadowDispatches, 1u);
	EXPECT_EQ(ConstrainedComputeContactTelemetry.ContactShadow.ContactShadowDraws, 0u);
	EXPECT_EQ(ConstrainedFragmentContactTelemetry.ContactShadow.ContactShadowDispatches, 0u);
	EXPECT_EQ(ConstrainedFragmentContactTelemetry.ContactShadow.ContactShadowDraws, 1u);
	// Isolated feature sweeps retain absolute timing as characterization output.
	// Their batches run under validation and do not share a frame-level GPU
	// clock or scheduling reference, so cross-batch absolute cost and tight
	// route ratios are not regression evidence. Relative feature benefits and
	// the synchronized production route below retain the hard timing gates.
	EXPECT_GT(FullGTAOFeatureMedian, 0u);
	EXPECT_GT(HalfGTAOFeatureMedian, 0u);
	EXPECT_GT(HalfGTAOResolveMedian, 0u);
	EXPECT_LE(HalfGTAOFeatureMedian * 100u, FullGTAOFeatureMedian * 65u);
	EXPECT_GT(ProductionGBufferMedian, 0u);
	EXPECT_GT(ProductionDeferredMedian, 0u);
	EXPECT_GT(ProductionRetainedOpaqueMedian, 0u);
	EXPECT_GT(ProductionVolumetricCloudMedian, 0u);
	EXPECT_GT(ProductionSortedTranslucencyMedian, 0u);
	EXPECT_GT(ProductionRetainedMedian, 0u);
	EXPECT_GT(ProductionPostProcessMedian, 0u);
	EXPECT_GT(ProductionShadowMedian, 0u);
	EXPECT_GT(ProductionTotalMedian, 0u);
	if (bNamedAdapter)
	{
		EXPECT_LE(ProductionGBufferMedian, 350'000u);
		EXPECT_LE(ProductionGBufferP95, 500'000u);
		EXPECT_LE(ProductionDeferredMedian, 450'000u);
		EXPECT_LE(ProductionDeferredP95, 650'000u);
		EXPECT_LE(ProductionAmbientOcclusionCompositionIncrement, 75'000u);
		EXPECT_LE(ProductionRetainedMedian, 300'000u);
		EXPECT_LE(ProductionRetainedP95, 500'000u);
		EXPECT_LE(ProductionPostProcessMedian, 100'000u);
		EXPECT_LE(ProductionPostProcessP95, 120'000u);
		EXPECT_LE(ProductionTotalMedian, 1'350'000u);
		EXPECT_LE(ProductionTotalP95, 3'000'000u);
		EXPECT_LE(ProductionTotalP95 * 100u, ProductionTotalMedian * 125u)
			<< "Qualification samples are unstable; rerun without competing GPU "
				"work before treating this as a renderer regression.";
	}
	constexpr uint64 ProductionActiveBytes =
		Durin::FPostProcessRenderer::CalculateSceneTargetBytes(
			TimingWidth, TimingHeight
		)
		+ Durin::FGBufferRenderer::CalculateTargetBytes(TimingWidth, TimingHeight)
		+ Durin::FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
			TimingWidth, TimingHeight,
			Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution
		)
		+ static_cast<uint64>(TimingWidth) * TimingHeight * 4u;
	constexpr uint64 ShadowBytes = 50'331'648u;
	constexpr uint64 ProductionRetainedCeiling =
		Durin::FPostProcessRenderer::MaximumRetainedSceneTargetBytes
		+ Durin::FGBufferRenderer::MaximumRetainedBytes
		+ Durin::FGroundTruthAmbientOcclusionRenderer::MaximumRetainedBytes;
	EXPECT_EQ(ProductionActiveBytes, 69'984'000u);
	EXPECT_EQ(ProductionActiveBytes + ShadowBytes, 120'315'648u);
	EXPECT_EQ(ProductionRetainedCeiling, 256u * 1024u * 1024u);
	EXPECT_EQ(ProductionRouteTelemetry.Deferred.DeferredDirectionalOutputBytes, 0u);
	EXPECT_EQ(
		ProductionRouteTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionAttemptedViews, 1u);
	EXPECT_EQ(
		ProductionRouteTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionEnabledViews, 1u);
	EXPECT_EQ(
		ProductionRouteTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionHalfResolutionViews,
		1u);
	EXPECT_EQ(
		ProductionRouteTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionUnavailableViews, 0u);
	EXPECT_EQ(ProductionRouteTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes,
		Durin::FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
			TimingWidth, TimingHeight,
			Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution));
	EXPECT_GE(
		ProductionRouteTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionRetainedBytes,
		ProductionRouteTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes);
	EXPECT_LE(
		ProductionRouteTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionRetainedBytes,
		Durin::FGroundTruthAmbientOcclusionRenderer::MaximumRetainedBytes);
	EXPECT_EQ(ProductionRouteTelemetry.VolumetricCloud.VolumetricCloudEnabledViews, 0u);
	EXPECT_EQ(ProductionRouteTelemetry.VolumetricCloud.VolumetricCloudDisabledViews, 1u);
	std::cout << "HYBRID_PRODUCTION_QUALIFICATION status="
			  << (bNamedAdapter ? "named_gate" : "observation")
			  << ",gpu=\"" << DeviceName << "\""
			  << ",driver=" << DeviceProperties.driverVersion
			  << ",vulkan=" << vk::apiVersionMajor(DeviceProperties.apiVersion)
			  << '.' << vk::apiVersionMinor(DeviceProperties.apiVersion)
			  << '.' << vk::apiVersionPatch(DeviceProperties.apiVersion)
			  << ",configuration=Win64-Debug-DurinEditor,validation=enabled"
			  << ",resolution=1920x1080,warmup_frames=" << WarmupFrames
			  << ",measured_frames=" << MeasuredFrames
			  << ",gbuffer_median_ns=" << ProductionGBufferMedian
			  << ",gbuffer_p95_ns=" << ProductionGBufferP95
			  << ",scene_median_ns=" << ProductionSceneMedian
			  << ",scene_p95_ns=" << ProductionSceneP95
			  << ",deferred_median_ns=" << ProductionDeferredMedian
			  << ",deferred_p95_ns=" << ProductionDeferredP95
			  << ",deferred_without_ao_median_ns="
			  << ProductionDeferredWithoutAOMedian
			  << ",ao_composition_increment_median_ns="
			  << ProductionAmbientOcclusionCompositionIncrement
			  << ",retained_opaque_median_ns="
			  << ProductionRetainedOpaqueMedian
			  << ",retained_opaque_p95_ns=" << ProductionRetainedOpaqueP95
			  << ",volumetric_cloud_median_ns="
			  << ProductionVolumetricCloudMedian
			  << ",volumetric_cloud_p95_ns=" << ProductionVolumetricCloudP95
			  << ",volumetric_cloud_enabled_views="
			  << ProductionRouteTelemetry.VolumetricCloud.VolumetricCloudEnabledViews
			  << ",sorted_translucency_median_ns="
			  << ProductionSortedTranslucencyMedian
			  << ",sorted_translucency_p95_ns="
			  << ProductionSortedTranslucencyP95
			  << ",retained_median_ns=" << ProductionRetainedMedian
			  << ",retained_p95_ns=" << ProductionRetainedP95
			  << ",fxaa_median_ns=" << ProductionPostProcessMedian
			  << ",fxaa_p95_ns=" << ProductionPostProcessP95
			  << ",shadow_median_ns=" << ProductionShadowMedian
			  << ",shadow_p95_ns=" << ProductionShadowP95
			  << ",contact_compute_median_ns="
			  << ProductionComputeContactMedian
			  << ",contact_compute_p95_ns="
			  << ProductionComputeContactP95
			  << ",contact_fragment_median_ns="
			  << ProductionFragmentContactMedian
			  << ",contact_fragment_p95_ns="
			  << ProductionFragmentContactP95
			  << ",contact_constrained_compute_median_ns="
			  << ConstrainedComputeContactMedian
			  << ",contact_constrained_compute_p95_ns="
			  << ConstrainedComputeContactP95
			  << ",contact_constrained_fragment_median_ns="
			  << ConstrainedFragmentContactMedian
			  << ",contact_constrained_fragment_p95_ns="
			  << ConstrainedFragmentContactP95
			  << ",total_median_ns=" << ProductionTotalMedian
			  << ",total_p95_ns=" << ProductionTotalP95
			  << ",full_gtao_feature_median_ns=" << FullGTAOFeatureMedian
			  << ",full_gtao_feature_p95_ns="
			  << FullGTAOFeatureP95
			  << ",half_gtao_feature_median_ns=" << HalfGTAOFeatureMedian
			  << ",half_gtao_feature_p95_ns="
			  << HalfGTAOFeatureP95
			  << ",half_gtao_resolve_median_ns=" << HalfGTAOResolveMedian
			  << ",half_gtao_resolve_p95_ns="
			  << HalfGTAOResolveP95
			  << ",active_bytes=" << ProductionActiveBytes
			  << ",active_with_shadow_bytes=" << ProductionActiveBytes + ShadowBytes
			  << ",retained_ceiling_bytes=" << ProductionRetainedCeiling << '\n';
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "HybridFourFamilyProduction", 320, 180,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 320;
			View.ViewportHeight = 180;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.DirectionalShadow.Candidate =
				Durin::EDirectionalShadowCandidate::SingleMap;
			Durin::FSceneViewRenderOptions Options;
			for (uint32 Frame = 0; Frame < 2; ++Frame)
			{
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		}
	);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GLastTelemetry.Deferred.HybridDeferredEnabledViews, 1u);
	EXPECT_EQ(GLastTelemetry.Deferred.HybridDeferredUnavailableViews, 0u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferStaticMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferSplineMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferSkeletalMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferTerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionAttemptedViews, 1u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionEnabledViews, 1u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes,
		Durin::FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
			320, 180,
			Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution));
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "GroundTruthAmbientOcclusionViewMatrix", 384, 216,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportX = 32;
			View.ViewportY = 18;
			View.ViewportWidth = 320;
			View.ViewportHeight = 180;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.AmbientOcclusion.bEnabled = false;
			Durin::FSceneViewRenderOptions Options;
			++Durin::GRenderFrameCounterRenderThread;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			EXPECT_EQ(
				GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionAttemptedViews, 0u
			);
			EXPECT_EQ(
				GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes, 0u
			);

			View.Settings.AmbientOcclusion.bEnabled = true;
			for (const auto Mode : {
					 Durin::EGroundTruthAmbientOcclusionDebugMode::Raw,
					 Durin::EGroundTruthAmbientOcclusionDebugMode::Confidence,
					 Durin::EGroundTruthAmbientOcclusionDebugMode::Filtered,
					 Durin::EGroundTruthAmbientOcclusionDebugMode::FinalFactor
				 })
			{
				Options.GroundTruthAmbientOcclusionDebugMode = Mode;
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		}
	);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionAttemptedViews, 1u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionEnabledViews, 1u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionDebugViews, 1u);
	EXPECT_EQ(GLastTelemetry.Deferred.DeferredDirectionalDebugViews, 0u);
	EXPECT_EQ(GLastTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes,
		Durin::FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
			384, 216,
			Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution));
	Scene.UpdatePrimitiveTransform(Durin::FPrimitiveSceneId(1), Translate(-0.9, -0.8));
	Scene.UpdatePrimitiveTransform(Durin::FPrimitiveSceneId(2), Translate(0.1, -0.9));
	Scene.UpdatePrimitiveTransform(Durin::FPrimitiveSceneId(3), Translate(-0.9, 0.1));
	Scene.UpdatePrimitiveTransform(Durin::FPrimitiveSceneId(4), Translate(0.1, 0.1));
	Directional.Intensity = 4.0f;
	Durin::FSceneInterfaceTestAccess::TryRemoveLightProxy(Scene, DirectionalToken);
	DirectionalToken = PublishLightForTest<Durin::FDirectionalLightSceneProxy>(
		Scene, Durin::FLightSceneId(100), Directional);
	ASSERT_TRUE(MaterialObject->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), {0.2, 0.55, 0.8}));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "HybridFourFamilyMutation", 384, 216,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 384;
			View.ViewportHeight = 216;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			Durin::FSceneViewRenderOptions Options;
			++Durin::GRenderFrameCounterRenderThread;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		}
	);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GLastTelemetry.Deferred.HybridDeferredEnabledViews, 1u);
	EXPECT_EQ(GLastTelemetry.GBuffer.GBufferSuccessfulDraws, 4u);
	GBufferQueries.clear();
	DeferredQueries.clear();

	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(1));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(2));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(3));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(4));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(5));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, Durin::FPrimitiveSceneId(6));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(SpecularAAScene, Durin::FPrimitiveSceneId(200));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&](Durin::FRHICommandListImmediate&) {
			StaticQuad->ReleaseResources();
			SpecularAAQuad->ReleaseResources();
			SkeletalQuad->ReleaseResources();
		}
	);
	Durin::FlushRenderingCommands();
	SpecularAASceneOwner.Reset();
	SceneOwner.Reset();
	RendererLifecycle.Shutdown();
	Durin::SetViewRenderTelemetrySink(nullptr);
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
