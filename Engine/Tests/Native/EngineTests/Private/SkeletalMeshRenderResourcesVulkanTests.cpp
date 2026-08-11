#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Renderers/SceneVisibility.h"
#include "Scene.h"
#include "SceneView.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

#include <array>
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
}

namespace
{
	Durin::FViewRenderCounters GLastCounters;
	auto CaptureCounters(const Durin::FViewRenderCounters& Counters) -> void
	{
		GLastCounters = Counters;
	}

	auto MakeRenderData(bool bComplete = true)
		-> std::unique_ptr<Durin::FSkeletalMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FSkeletalMeshRenderData>();
		const std::vector<Durin::FVector3f> Positions{
			{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
		Data->VertexBuffers.Geometry.PositionVertexBuffer.Init(Positions);
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(3, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(3, {1.0f, 0.0f, 0.0f, 1.0f}));
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels> UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), 3, 1);
		Data->VertexBuffers.Geometry.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(3, Durin::FVector4f(1.0f)), 3);
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
				{First, Mixed, Second});
		}
		Data->IndexBuffer.Init({0, 1, 2, 0, 1, 2, 0, 1, 2});
		for (Durin::uint32 SectionIndex = 0; SectionIndex < 3; ++SectionIndex)
			Data->Sections.push_back({
				.Name = Durin::FName(std::format("Section{}", SectionIndex)),
				.FirstIndex = SectionIndex * 3, .IndexCount = 3,
				.MinVertexIndex = 0, .MaxVertexIndex = 2,
				.MaterialSlotIndex = SectionIndex,
				.LocalBounds = Durin::FBox(
					{0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})});
		Data->MaterialSlots = {
			Durin::FName("Opaque"), Durin::FName("Masked"),
			Durin::FName("Translucent")};
		Data->PaletteBoneIndices = {0, 1};
		Data->InverseBindMatrices = {
			Durin::FMatrix4f(1.0f), Durin::FMatrix4f(1.0f)};
		Data->InfluenceBounds = {
			Durin::FBox({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}),
			Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})};
		Data->LocalBounds = Data->Sections[0].LocalBounds;
		return Data;
	}

	struct FSkeletalResourceLifecycleCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "SkeletalResourceLifecycle";
		}
	};
}

TEST(FSkeletalMeshRenderResourcesVulkanTests, InitializesRejectsRetriesAndReleasesExactly)
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
	const Durin::FRHICapabilities* Capabilities =
		Durin::GDynamicRHI->RHIGetCapabilities();
	ASSERT_NE(Capabilities, nullptr);
	EXPECT_GT(Capabilities->MinStorageBufferOffsetAlignment, 0u);
	EXPECT_GE(Capabilities->MaxStorageBufferRange, sizeof(Durin::FMatrix4f));
	Durin::InitRenderingThread();
	Durin::FRendererModule Renderer;
	Renderer.StartupModule();
	Durin::SetViewRenderCounterSink(CaptureCounters);

	auto Complete = MakeRenderData();
	auto Malformed = MakeRenderData(false);
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			Durin::FShaderCompileOptions CompileOptions;
			CompileOptions.Macros.emplace_back("DURIN_SKELETAL_MESH", "1");
			CompileOptions.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", "0");
			CompileOptions.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", "0");
			CompileOptions.Macros.emplace_back(
				"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", "0");
			Durin::FShaderType& VertexShaderType =
				Durin::FTestSkeletalMeshVertexShader::StaticType();
			const std::array<const Durin::FShaderType*, 1> ShaderTypes{
				&VertexShaderType};
			Durin::FShaderMapBase ShaderMap;
			std::string ShaderError;
			ASSERT_TRUE(ShaderMap.InitializeFromShaderTypes(
				ShaderTypes, CompileOptions, ShaderError)) << ShaderError;
			const auto* VertexShader = static_cast<Durin::FTestSkeletalMeshVertexShader*>(
				ShaderMap.GetShader(&VertexShaderType));
			ASSERT_NE(VertexShader, nullptr);
			const auto& ParameterBindings = VertexShader->GetParameterBindings();
			ASSERT_EQ(ParameterBindings.size(), 2u);
			EXPECT_STREQ(ParameterBindings[0].Name, "Transform");
			EXPECT_STREQ(ParameterBindings[1].Name, "SkinPalette");
			EXPECT_EQ(
				ParameterBindings[1].Type,
				Durin::ERHIBindingType::StorageBuffer);

			const std::array<Durin::FMatrix4f, 2> Palette{
				Durin::FMatrix4f(1.0f), Durin::FMatrix4f(2.0f)};
			const auto FirstRange = CommandList.AllocateDynamicStorageBuffer(
				Palette.data(), sizeof(Palette));
			const auto SecondRange = CommandList.AllocateDynamicStorageBuffer(
				Palette.data(), sizeof(Palette));
			ASSERT_NE(FirstRange.Buffer, nullptr);
			ASSERT_NE(SecondRange.Buffer, nullptr);
			EXPECT_EQ(FirstRange.Size, sizeof(Palette));
			EXPECT_EQ(SecondRange.Size, sizeof(Palette));
			EXPECT_EQ(
				FirstRange.Offset % Capabilities->MinStorageBufferOffsetAlignment,
				0u);
			EXPECT_EQ(
				SecondRange.Offset % Capabilities->MinStorageBufferOffsetAlignment,
				0u);
			EXPECT_TRUE(
				FirstRange.Buffer != SecondRange.Buffer
				|| FirstRange.Offset + FirstRange.Size <= SecondRange.Offset
				|| SecondRange.Offset + SecondRange.Size <= FirstRange.Offset);
			const std::array<Durin::FRHIBufferTransition, 2> PaletteTransitions{
				Durin::FRHIBufferTransition{
					.Buffer = FirstRange.Buffer,
					.Offset = FirstRange.Offset,
					.Size = FirstRange.Size,
					.ExpectedBefore = Durin::ERHIAccess::HostWrite,
					.RequiredAfter = Durin::ERHIAccess::GraphicsShaderRead},
				Durin::FRHIBufferTransition{
					.Buffer = SecondRange.Buffer,
					.Offset = SecondRange.Offset,
					.Size = SecondRange.Size,
					.ExpectedBefore = Durin::ERHIAccess::HostWrite,
					.RequiredAfter = Durin::ERHIAccess::GraphicsShaderRead}};
			CommandList.TransitionBuffers(PaletteTransitions);

			if (Capabilities->MaxStorageBufferRange < UINT32_MAX)
			{
				const std::uint8_t Sentinel = 0;
				const auto RejectedRange = CommandList.AllocateDynamicStorageBuffer(
					&Sentinel, Capabilities->MaxStorageBufferRange + 1u);
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
		});
	Durin::FlushRenderingCommands();

	auto MakeMaterial = [](Durin::EMaterialBlendMode BlendMode,
		Durin::FVector3 Color) {
		auto Material = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
		Durin::FMaterialRenderProxyPublication Publication;
		Publication.LocalVersion = 1;
		Publication.LocalLayer.StaticProperties =
			Durin::FMaterialStaticProperties{
				.BlendMode = BlendMode,
				.ShadingModel = Durin::EMaterialShadingModel::Unlit,
				.bTwoSided = true};
		Publication.LocalLayer.Parameters.push_back({
			.Id = Durin::MaterialParameters::BaseColorId,
			.Type = Durin::EMaterialParameterType::Vector,
			.VectorValue = Color});
		EXPECT_TRUE(Material->QueuePublication_GameThread(std::move(Publication)));
		return Material;
	};
	auto Opaque = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque, {1.0, 0.0, 0.0});
	auto Masked = MakeMaterial(
		Durin::EMaterialBlendMode::Masked, {0.0, 1.0, 0.0});
	auto Translucent = MakeMaterial(
		Durin::EMaterialBlendMode::Translucent, {0.0, 0.0, 1.0});
	auto Pose = std::make_shared<Durin::FSkeletalPosePalette>();
	Pose->Revision = 1;
	Pose->SkeletonCompatibilityIdentity = "VulkanSkeletalMesh";
	Pose->Matrices = {Durin::FMatrix4f(1.0f), Durin::FMatrix4f(1.0f)};
	Pose->LocalBounds = Complete->LocalBounds;
	Durin::FScene Scene;
	Scene.AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(1),
		std::make_unique<Durin::FSkeletalMeshSceneProxy>(
			Complete.get(), std::vector<Durin::FMaterialRenderProxyRef>{
				Opaque, Masked, Translucent},
			1, Pose),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();

	auto Readback = std::make_shared<std::vector<Durin::uint8>>();
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&Renderer, &Scene, Readback](Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const Durin::FRHITextureCreateDesc Desc =
				Durin::FRHITextureCreateDesc::Create2D(
					"SkeletalMeshValidationColor", 33, 33,
					Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource
					| Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 33;
			View.ViewportHeight = 33;
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			(void)Renderer.RenderView(
				CommandList, &Scene, View, Target, false, {});
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Target, 0, 0, *Readback));
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
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
	EXPECT_EQ(GLastCounters.UploadedSkeletalPaletteBytes,
		2u * sizeof(Durin::FMatrix4f));
	EXPECT_EQ(GLastCounters.SkeletalMeshResourceAttemptedDraws, 3u);
	EXPECT_EQ(GLastCounters.SkeletalMeshResourceSuccessfulDraws, 3u);
	EXPECT_EQ(GLastCounters.SkeletalMeshSuccessfulDraws, 3u);
	EXPECT_TRUE(std::ranges::any_of(*Readback,
		[](Durin::uint8 Value) { return Value != 0; }));
	size_t RedPixels = 0;
	for (size_t Offset = 0; Offset + 3 < Readback->size(); Offset += 4)
		RedPixels += (*Readback)[Offset] > (*Readback)[Offset + 1] + 20
			&& (*Readback)[Offset] > (*Readback)[Offset + 2] + 20 ? 1u : 0u;
	EXPECT_GT(RedPixels, 0u);
	auto TranslatedPose = std::make_shared<Durin::FSkeletalPosePalette>(*Pose);
	TranslatedPose->Revision = 2;
	TranslatedPose->Matrices[1] = glm::scale(
		Durin::FMatrix4f(1.0f), Durin::FVector3f(0.5f, 1.5f, 1.0f));
	TranslatedPose->LocalBounds = Durin::FBox(
		Pose->LocalBounds.Min * 0.5, Pose->LocalBounds.Max * 0.5);
	Scene.UpdateSkeletalMeshDynamicData(
		Durin::FPrimitiveSceneId(1), TranslatedPose);
	Durin::FlushRenderingCommands();
	auto TranslatedReadback = std::make_shared<std::vector<Durin::uint8>>();
	auto AuxiliaryReadback = std::make_shared<std::vector<Durin::uint8>>();
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&Renderer, &Scene, TranslatedReadback, AuxiliaryReadback](
			Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"TranslatedSkeletalMeshValidationColor", 33, 33,
				Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource
					| Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 33;
			View.ViewportHeight = 33;
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			(void)Renderer.RenderView(
				CommandList, &Scene, View, Target, false, {});
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Target, 0, 0, *TranslatedReadback));

			const auto AuxiliaryDesc = Durin::FRHITextureCreateDesc::Create2D(
				"AuxiliarySkeletalMeshValidationColor", 48, 27,
				Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource
					| Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef AuxiliaryTarget =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, AuxiliaryDesc);
			ASSERT_NE(AuxiliaryTarget, nullptr);
			Durin::FSceneView AuxiliaryView;
			AuxiliaryView.ViewProjectionMatrix = Durin::FMatrix(1.0);
			AuxiliaryView.ViewportWidth = 48;
			AuxiliaryView.ViewportHeight = 27;
			AuxiliaryView.AspectRatioConstraint = 16.0f / 9.0f;
			AuxiliaryView.Settings.RenderMode = Durin::ERenderMode::Lit;
			AuxiliaryView.Settings.RasterMode = Durin::ERasterMode::Wireframe;
			AuxiliaryView.Settings.bEnableFXAA = false;
			AuxiliaryView.Settings.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			(void)Renderer.RenderView(CommandList, &Scene, AuxiliaryView,
				AuxiliaryTarget, false, {});
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, AuxiliaryTarget, 0, 0, *AuxiliaryReadback));
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	EXPECT_EQ(TranslatedReadback->size(), Readback->size());
	EXPECT_NE(*TranslatedReadback, *Readback);
	EXPECT_EQ(AuxiliaryReadback->size(), 48u * 27u * 4u);
	EXPECT_NE(AuxiliaryReadback->size(), TranslatedReadback->size());

	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(1));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FSkeletalResourceLifecycleCommand>(
		[&Complete](Durin::FRHICommandListImmediate&) {
			Complete->ReleaseResources();
		});
	Durin::FlushRenderingCommands();
	Renderer.ShutdownModule();
	Durin::SetViewRenderCounterSink(nullptr);
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
