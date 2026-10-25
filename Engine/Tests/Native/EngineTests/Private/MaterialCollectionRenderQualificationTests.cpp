#include "CoreMinimal.h"
#include <gtest/gtest.h>
#include "GeometrySubmissionTestSupport.h"
#include "LightSceneTestSupport.h"
#include "EngineTestSupport.h"
#include "NativeTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressions.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialRenderProxy.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPaths.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RendererModule.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/ViewRenderTelemetry.h"
#include "SceneInfo.h"
#include "SceneTestAccess.h"
#include "SceneView.h"
#include "VulkanEngineTestSupport.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

namespace
{
	struct FCaptureMaterialCollectionViewCommand
	{
		static constexpr auto GetName() -> const char* { return "CaptureMaterialCollectionView"; }
	};
}

TEST(FMaterialCollectionRenderQualificationTests, RecordsStableWorldRenderViewBaseline)
{
	using namespace Durin;
	using namespace Durin::Tests;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(InitializeAssetCompilingManager());
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(InitializeAssetManager());
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness RendererLifecycle("MaterialCollectionQualification");
	RendererLifecycle.Start(Renderer);
	{
		TStrongObjectPtr<DMaterialParameterCollection> Collection(
			NewObject<DMaterialParameterCollection>(nullptr, "GPUCollection"));
		const FGuid ColorId = FGuid::NewGuid();
		const FGuid MaskId = FGuid::NewGuid();
		const std::array Declarations{
			FMaterialParameterCollectionDeclaration{.Id = ColorId,
				.Name = FName("Color"), .Type = EMaterialParameterType::Vector,
				.DefaultValue = FVector4(1.0, 0.0, 0.0, 0.0)},
			FMaterialParameterCollectionDeclaration{.Id = MaskId,
				.Name = FName("Mask"), .Type = EMaterialParameterType::Scalar,
				.DefaultValue = FVector4(1.0, 0.0, 0.0, 0.0)}};
		ASSERT_TRUE(Collection->SetDeclarations(Declarations));
		auto Layout = Collection->BuildLayout();
		ASSERT_TRUE(Layout);

		TStrongObjectPtr<DMaterial> Material(
			NewObject<DMaterial>(nullptr, "GPUCollectionMaterial"));
		TStrongObjectPtr<DMaterialExpressionCollectionParameter> Color(
			NewObject<DMaterialExpressionCollectionParameter>(nullptr, "GPUCollectionColor"));
		TStrongObjectPtr<DMaterialExpressionCollectionParameter> Mask(
			NewObject<DMaterialExpressionCollectionParameter>(nullptr, "GPUCollectionMask"));
		Color->Id = FGuid::NewGuid(); Color->Collection = Collection.Get();
		Color->ParameterId = ColorId;
		Mask->Id = FGuid::NewGuid(); Mask->Collection = Collection.Get();
		Mask->ParameterId = MaskId;
		FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.BaseColor = {Color->Id};
		Outputs.OpacityMask = {Mask->Id};
		const std::array<DMaterialExpression*, 2> Expressions{Color.Get(), Mask.Get()};
		FMaterialStaticProperties Properties;
		Properties.BlendMode = EMaterialBlendMode::Masked;
		Properties.OpacityMaskThreshold = 0.5f;
		Properties.bTwoSided = true;
		ASSERT_TRUE(Material->SetStaticProperties(Properties));
		ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		ASSERT_EQ(Material->GetMaterialCompileStatus().State,
			EMaterialCompileState::Ready);

		auto MakeSnapshot = [&](FVector4 ColorValue, float MaskValue, uint64 Version) {
			auto Snapshot = std::make_shared<FMaterialParameterCollectionSnapshot>();
			Snapshot->Layout = *Layout;
			Snapshot->Version = Version;
			Snapshot->Payload = Layout->DefaultPayload;
			auto Write = [&](FGuid Id, const FVector4& Value) {
				const auto Field = std::ranges::find(
					Layout->UniformLayout.Fields, Id,
					&FMaterialRenderField::ParameterId);
				if (Field == Layout->UniformLayout.Fields.end())
				{
					ADD_FAILURE() << "Missing collection field";
					return;
				}
				const std::array<float, 4> Components{
					static_cast<float>(Value.x), static_cast<float>(Value.y),
					static_cast<float>(Value.z), static_cast<float>(Value.w)};
				std::memcpy(Snapshot->Payload.data() + Field->Offset,
					Components.data(), Field->Size);
			};
			Write(ColorId, ColorValue);
			Write(MaskId, FVector4(MaskValue, 0.0, 0.0, 0.0));
			return Snapshot;
		};

		FSceneTestOwner RedWorld;
		FSceneTestOwner GreenWorld;
		RedWorld->UpdateMaterialParameterCollection(
			MakeSnapshot(FVector4(1.0, 0.0, 0.0, 0.0), 1.0f, 1));
		GreenWorld->UpdateMaterialParameterCollection(
			MakeSnapshot(FVector4(0.0, 1.0, 0.0, 0.0), 1.0f, 1));
		const auto MaterialProxy = Material->GetRenderProxy();
		FProceduralGeometry Geometry;
		EnqueueRenderCommand<FCaptureMaterialCollectionViewCommand>(
			[&](FRHICommandListImmediate& Commands) {
				Geometry = FProceduralGeometry::Create(Commands, 901, false);
				const auto Resolved = MaterialProxy->GetMaterialWithFallback();
				for (FScene* Scene : {RedWorld.Get(), GreenWorld.Get()})
				{
					auto Proxy = std::make_unique<FProceduralProxy>();
					Proxy->Geometry = Geometry;
					Proxy->Materials = {Resolved, Resolved};
					ASSERT_TRUE(Proxy->PublishGeometry());
					FMatrix Transform(1.0);
					Transform[0][0] = Transform[1][1] = Transform[2][2] = 0.8;
					Transform[3][0] = 0.05;
					FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene,
						FPrimitiveComponentId(901), std::move(Proxy), Transform);
				}
			});
		FlushRenderingCommands();
		for (FScene* Scene : {RedWorld.Get(), GreenWorld.Get()})
		{
			FDirectionalLightSceneData Light;
			Light.Intensity = 3.0f;
			ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(
				*Scene, FLightComponentId(901), Light), nullptr);
		}
		FlushRenderingCommands();
		{
			const auto Root = Testing::CreateTestFixtureDirectory("MaterialCollectionBaseline");
			std::string Csv = "run,sample,render_view_ns,command_hits,command_misses,surface_preparations,binding_batches\n";
			EnqueueRenderCommand<FCaptureMaterialCollectionViewCommand>([&](FRHICommandListImmediate& Commands) {
				FSceneView View;
				View.ViewportWidth = 192; View.ViewportHeight = 108;
				View.Settings.Mode.RenderMode = ERenderMode::Unlit;
				View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
				const auto Target = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("CollectionBenchmark", 192, 108, EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource));
				ASSERT_TRUE(Target);
				static FViewRenderTelemetry Telemetry;
				SetViewRenderTelemetrySink(+[](const FViewRenderTelemetry& Value) { Telemetry = Value; });
				for (uint32 Run = 0; Run < 3; ++Run)
				{
					std::vector<int64> Samples;
					for (int32 Sample = -30; Sample < 120; ++Sample)
					{
						++GRenderFrameCounterRenderThread;
						GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
						const auto Begin = std::chrono::steady_clock::now();
						const auto Result = Renderer.RenderView(Commands, {RedWorld.Get(), View, Target, false, {}});
						const auto Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin).count();
						EXPECT_EQ(Result, ERenderViewResult::Success);
						GDynamicRHI->RHIEndFrame_RenderThread(Commands);
						if (Sample < 0) continue;
						Samples.push_back(Ns);
						Csv += std::format("{},{},{},{},{},{},{}\n", Run, Sample, Ns,
							Telemetry.StaticMesh.PersistentCommandHits, Telemetry.StaticMesh.PersistentCommandMisses,
							Telemetry.StaticMesh.SurfaceMaterialPreparations, Telemetry.StaticMesh.PreparedSurfaceBindingBatches);
					}
					std::ranges::sort(Samples);
					std::cout << "[MaterialCollectionBaseline] run=" << Run << " median_ns=" << Samples[59]
						<< " p95_ns=" << Samples[113] << '\n';
				}
				SetViewRenderTelemetrySink(nullptr);
			});
			FlushRenderingCommands();
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Csv)), Root / "samples.csv"));
			RecordProperty("authority", "diagnostic-host-build");
			std::cout << "[MaterialCollectionBaseline] csv=" << (Root / "samples.csv").generic_string() << '\n';
		}
		RedWorld.Reset();
		GreenWorld.Reset();
	}
	FlushRenderingCommands();
	RendererLifecycle.Shutdown();
	ShutdownRenderingThread();
	RHIExit();
}
