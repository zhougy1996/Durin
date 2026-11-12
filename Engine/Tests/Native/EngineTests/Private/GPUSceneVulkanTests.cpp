#include "GPUScene.h"
#include "SceneInfo.h"
#include "SceneTestAccess.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Renderers/RendererRDGAllocator.h"
#include "Resources/RendererResourceCoordinator.h"
#include "VulkanEngineTestSupport.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/MountPaths.h"
#include <gtest/gtest.h>

namespace Durin
{
	namespace
	{
		struct FGPUSceneReadbackParameters
		{
			FRDGBufferAccess Source;
			FRDGTextureAccess Destination;
			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using P = FGPUSceneReadbackParameters;
				static const std::array Members{
					MakeRDGBufferAccessMetadata<P, decltype(Source)>("Source", offsetof(P, Source)),
					MakeRDGTextureAccessMetadata<P, decltype(Destination)>("Destination", offsetof(P, Destination))};
				static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FGPUSceneReadbackParameters", Members);
				return &Metadata;
			}
		};
		struct FGPUSceneNativeCommand { static constexpr auto GetName() -> const char* { return "GPUSceneNativeUpload"; } };
	}

	TEST(FGPUSceneVulkanTests, UploadsPatchesGrowthAndRecoveryMatchMirrorBytes)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
		FModuleManager::Get().LoadModule("RenderCore");
		ASSERT_EQ(GDynamicRHI, nullptr);
		RHIInit(Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		{
			FSceneTestOwner Scene;
			FStaticMeshRenderData RenderData;
			RenderData.LocalBounds = FBox(FVector3(-1.0), FVector3(1.0));
			auto Add = [&](uint64 Id) {
				FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId{Id},
					std::make_unique<FStaticMeshSceneProxy>(&RenderData, std::vector<FMaterialRenderProxyRef>{}),
					Math::TranslationMatrix(FVector3(1e12 + Id, 0.0, 0.0)), false);
			};
			auto Run = [&](bool ExpectUpload, bool Recover = false, bool Reject = false) {
				EnqueueRenderCommand<FGPUSceneNativeCommand>([&](FRHICommandListImmediate& Commands) {
					FRendererResourceCoordinator Coordinator;
					FRendererRDGAllocator Allocator(Coordinator);
					if (Recover) Scene->GPUScene->ReleaseRHI();
					FRDGBuilder Graph;
					Graph.EnablePassCulling();
					auto Update = Scene->GPUScene->Update(Graph);
					if (Reject)
					{
						FRDGBudget Budget; Budget.MaxPasses = 0; Graph.SetBudget(Budget);
						EXPECT_FALSE(Graph.Execute(Commands, &Allocator));
						EXPECT_FALSE(Scene->GPUScene->CompleteUpdate(Graph, Update));
						return;
					}
					EXPECT_EQ(Update.UploadBytes != 0, ExpectUpload);
					if (Recover) EXPECT_EQ(Update.BufferRebuilds, 3u);
					const auto SceneParameters = Scene->GPUScene->GetShaderParameters(Graph);
					const std::array Buffers{SceneParameters.GPUScenePrimitiveSceneData,
						SceneParameters.GPUSceneInstanceSceneData, SceneParameters.GPUSceneInstancePayloadData};
					std::array<FTextureRHIRef, 3> Readback;
					for (uint32 Index = 0; Index < Buffers.size(); ++Index)
					{
						const uint32 Width = static_cast<uint32>(Buffers[Index].Size / 4);
						const auto Texture = Graph.CreateTexture({.Texture = FRHITextureCreateDesc::Create2D(
							"GPUSceneReadback", Width, 1, EPixelFormat::RGBA8_UNORM)
							.SetFlags(ETextureCreateFlags::SourceCopy | ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback)}, std::format("GPUScene.Readback{}", Index));
						auto Parameters = Graph.AllocParameters<FGPUSceneReadbackParameters>();
						Parameters->Source = {Buffers[Index].Buffer, 0, Buffers[Index].Size, ERDGUse::Read, ERHIAccess::TransferRead};
						Parameters->Destination = {Texture, {.Aspects = ERHITextureAspect::Color, .NumMips = 1, .NumArrayLayers = 1}, ERDGUse::Write, ERHIAccess::TransferWrite, true};
						Graph.AddPass(std::format("GPUScene.Copy{}", Index), ERDGPassType::Copy, std::move(Parameters),
							[Width](FRHICommandListImmediate& List, const FGPUSceneReadbackParameters& P, const FRDGParameterResolver& Resolver) {
								const std::array Regions{FRHIBufferTextureCopyRegion{.TextureExtent = {Width, 1, 1}}};
								List.CopyBufferToTexture(Resolver.GetBuffer(P.Source), Resolver.GetTexture(P.Destination), Regions);
							});
						Graph.QueueTextureExtraction(Texture, &Readback[Index], ERHIAccess::TransferRead);
					}
					const auto Executed = Graph.Execute(Commands, &Allocator);
					ASSERT_TRUE(Executed) << ToString(Executed.error());
					ASSERT_TRUE(Scene->GPUScene->CompleteUpdate(Graph, Update));
					const std::array Expected{
						std::as_bytes(Scene->GPUScene->GetPrimitiveSceneData()),
						std::as_bytes(Scene->GPUScene->GetInstanceSceneData()),
						std::as_bytes(Scene->GPUScene->GetInstancePayloadData())};
					for (uint32 Index = 0; Index < Readback.size(); ++Index)
					{
						FByteBuffer Bytes;
						ASSERT_TRUE(Commands.ReadTexture2D(Readback[Index], 0, 0, Bytes));
						ASSERT_GE(Bytes.size(), Expected[Index].size());
						EXPECT_TRUE(std::ranges::equal(Expected[Index], std::span(Bytes).first(Expected[Index].size())));
						EXPECT_TRUE(std::ranges::all_of(std::span(Bytes).subspan(Expected[Index].size()), [](std::byte Byte) { return Byte == std::byte{0}; }));
					}
					Allocator.Release_RenderThread();
				});
				FlushRenderingCommands();
			};
			Add(1); Run(true, false, true); Run(true); Run(false);
			FPrimitiveCustomData Custom; Custom.Values[4] = 42.f;
			Scene->UpdatePrimitiveCustomData(FPrimitiveComponentId{1}, Custom);
			Run(true); Run(false);
			Add(2); Add(3); Run(true); Run(false);
			auto Instances = std::make_shared<std::vector<FStaticMeshRenderInstance>>();
			Instances->push_back({10, Math::TranslationMatrix(FVector3(7.0, 0.0, 0.0)), Custom});
			Instances->push_back({20, FMatrix(1.0), {}});
			Scene->UpdateStaticMeshRenderInstances(FPrimitiveComponentId{1}, Instances);
			Run(true); Run(false); Run(true, true); Run(false);
            FGPUSceneDynamicAllocation Dynamic;
            EnqueueRenderCommand<FGPUSceneNativeCommand>([&](FRHICommandListImmediate&) {
                Dynamic = Scene->GPUScene->AllocateDynamicPrimitiveRange(4);
                for (uint32 Index = 0; Index < Dynamic.Count; ++Index) {
                    FPrimitiveCustomData PrimitiveData, InstanceData; PrimitiveData.Values[2] = 11.f + Index; InstanceData.Values[7] = 9.f + Index;
                    EXPECT_TRUE(Scene->GPUScene->WriteDynamicPrimitive(Dynamic, Index,
                        Math::TranslationMatrix(FVector3(1e12 + 13.0 + Index, 0.0, 0.0)), RenderData.LocalBounds, PrimitiveData, InstanceData));
                }
            });
            FlushRenderingCommands(); Run(true); Run(false);
            EnqueueRenderCommand<FGPUSceneNativeCommand>([&](FRHICommandListImmediate&) { Scene->GPUScene->ReleaseDynamicPrimitiveRange(Dynamic); });
            FlushRenderingCommands(); Run(false);
            Add(4); Run(true); Run(false);
			Scene.Reset();
			FlushRenderingCommands();
		}
		ShutdownRenderingThread();
		RHIExit();
	}
}
