#include "EngineTestSupport.h"
#include "Actors/ProceduralSkyActor.h"
#include "Actors/SkyLightActor.h"
#include "Asset/AssetCompilingManager.h"
#include "Components/ProceduralSkyComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Image/Image.h"
#include "Misc/MountPathTestSupport.h"
#include "RendererModule.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "SceneTestAccess.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Texture/TextureCube.h"
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>

namespace
{
    class FMetalSkyEnvironment
    {
    public:
        FMetalSkyEnvironment(const char* BackendName,const char* Mode)
        {
            if (const char* Value = std::getenv("DURIN_RHI_BACKEND")) Backend = Value;
            if (const char* Value = std::getenv("DURIN_RHI_EXECUTION")) Execution = Value;
            setenv("DURIN_RHI_BACKEND", BackendName, 1);
            setenv("DURIN_RHI_EXECUTION", Mode, 1);
        }
        ~FMetalSkyEnvironment()
        {
            if (Backend) setenv("DURIN_RHI_BACKEND", Backend->c_str(), 1);
            else unsetenv("DURIN_RHI_BACKEND");
            if (Execution) setenv("DURIN_RHI_EXECUTION", Execution->c_str(), 1);
            else unsetenv("DURIN_RHI_EXECUTION");
        }
    private:
        std::optional<std::string> Backend, Execution;
    };
}

TEST(FMetalSkyLightingTests, CapturedAndSpecifiedSourcesMatchEnergyAcrossBackends)
{
    using namespace Durin;
    InitializeDObjectSystem();
    ASSERT_TRUE(InitializeAssetCompilingManager());
    ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
    ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
    ASSERT_TRUE(FModuleManager::Get().LoadModule("TextureBuild"));
    for (const char* BackendName : {"metal", "vulkan"})
    for (const char* Mode : {"inline", "threaded"})
    {
        SCOPED_TRACE(BackendName);
        SCOPED_TRACE(Mode);
        FMetalSkyEnvironment Environment(BackendName,Mode);
        ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
        ASSERT_TRUE(GDynamicRHI->RHIGetCapabilities()->bSupportsSkyLighting);
        EXPECT_TRUE(GDynamicRHI->RHIGetCapabilities()->bSupportsGPUTimestamps);
        InitRenderingThread();
        FRendererModule Renderer;
        FModuleTestHarness Lifecycle("MetalSkyRenderer");
        Lifecycle.Start(Renderer);
        auto SceneOwner = Renderer.CreateScene();
        auto& Scene = static_cast<FScene&>(*SceneOwner);
        auto* World = NewObject<DWorld>(nullptr, "MetalSkyWorld");
        AddToRoot(World);
        ASSERT_TRUE(World->InitializeSubsystems());
        World->SetRenderScene(&Scene);
        ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "SkyLevel")));
        auto* Sky = World->SpawnActor<AProceduralSkyActor>("Sky")->GetSkyComponent();
        auto* Light = World->SpawnActor<ASkyLightActor>("Light")->GetSkyLightComponent();
        Sky->SetRadianceColors({2,4,8}, {2,4,8}, {2,4,8}, {0,0,0});
        Light->SetSource(ESkyLightSourceMode::CapturedSky, nullptr);
        Light->SetRefreshPolicy(false, 0.25f);
        bool Ready = false;
        uint64 Request = 0;
        const auto Pump = [&]() {
            PumpGameThreadDeferredWork();
            EnqueueRenderCommand("BeginMetalSkyFrame", [](FRHICommandListImmediate& Cmd) {
                ++GRenderFrameCounterRenderThread;
                GDynamicRHI->RHIBeginFrame_RenderThread(Cmd);
            });
            World->Tick({.DeltaSeconds = 0.02f});
            EnqueueRenderCommand("EndMetalSkyFrame", [&](FRHICommandListImmediate& Cmd) {
                Renderer.UpdateScenes_RenderThread(Cmd);
                Ready = bool(Scene.SkyLighting->Active);
                Request = Ready ? Scene.SkyLighting->Active->Request : 0;
                GDynamicRHI->RHIEndFrame_RenderThread(Cmd);
            });
            FlushRenderingCommands();
        };
        const auto WaitForReady = [&]() {
            for (int Poll = 0; Poll < 200 && !Ready; ++Poll)
            {
                Pump();
                if (!Ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            EXPECT_TRUE(Ready);
            EXPECT_EQ(Light->GetUpdateStatus()->State.load(), ESkyLightUpdateState::Ready);
        };
        const auto CheckEnergy = [&](const FVector3f& Radiance, bool Captured) {
            EnqueueRenderCommand("ReadMetalSkyEnergy", [&](FRHICommandListImmediate& Cmd) {
                ASSERT_TRUE(Scene.SkyLighting->Active);
                const auto& G = *Scene.SkyLighting->Active;
                EXPECT_EQ(bool(G.Radiance), Captured);
                EXPECT_EQ(G.Prefiltered->GetNumMips(), 8u);
                const auto Check = [&](FRHITexture* Texture, uint32 Mip, uint32 Face, float Scale) {
                    FByteBuffer Pixels;
                    ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd, Texture, Mip, Face, Pixels));
                    ASSERT_FALSE(Pixels.empty());
                    for (const size_t Offset : {size_t{0}, (Pixels.size() / 16) * 8, Pixels.size() - 8})
                        for (uint32 Channel = 0; Channel < 3; ++Channel)
                        {
                            uint16 Bits;
                            std::memcpy(&Bits, Pixels.data() + Offset + Channel * 2, 2);
                            EXPECT_NEAR(glm::unpackHalf2x16(uint32(Bits)).x,
                                Radiance[Channel] * Scale, 0.03f);
                        }
                };
                for (uint32 Face = 0; Face < 6; ++Face)
                {
                    Check(G.Irradiance, 0, Face, 3.14159265f);
                    for (uint32 Mip = 0; Mip < 8; ++Mip)
                    {
                        Check(G.Prefiltered, Mip, Face, 1);
                        if (Captured) Check(G.Radiance, Mip, Face, 1);
                    }
                }
            });
            FlushRenderingCommands();
        };
        WaitForReady();
        if (Ready) CheckEnergy({2,4,8}, true);
        // Exhaust optional diagnostics; completion must still allow manual refresh.
        std::vector<FGPUTimingQueryRHIRef> HeldQueries;
        EnqueueRenderCommand("ExhaustSkyTimingPool", [&](FRHICommandListImmediate&) {
            for (uint32 Index = 0; Index < 1500; ++Index)
            {
                auto Query = GDynamicRHI->RHICreateGPUTimingQuery();
                if (!Query) break;
                HeldQueries.push_back(std::move(Query));
            }
            ASSERT_FALSE(GDynamicRHI->RHICreateGPUTimingQuery());
        });
        FlushRenderingCommands();
        const auto FirstRequest = Request;
        Sky->SetRadianceColors({1,3,5}, {1,3,5}, {1,3,5}, {0,0,0});
        Light->Recapture();
        for (int Poll = 0; Poll < 200 && Request == FirstRequest; ++Poll)
        {
            Pump();
            if (Request == FirstRequest) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_NE(Request, FirstRequest);
        EXPECT_GT(Light->GetUpdateStatus()->CompletedUpdates.load(), 0u);
        EXPECT_GT(Light->GetUpdateStatus()->UpdateMilliseconds.load(), 0.0);
        if (Ready) CheckEnergy({1,3,5}, true);
        const auto BeforeUntimedCompletion = Light->GetUpdateStatus()->CompletedUpdates.load();
        for (int Poll = 0; Poll < 200 && Light->GetUpdateStatus()->CompletedUpdates.load() == BeforeUntimedCompletion; ++Poll)
        {
            Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_GT(Light->GetUpdateStatus()->CompletedUpdates.load(), BeforeUntimedCompletion);
        EXPECT_EQ(Light->GetUpdateStatus()->UpdateMilliseconds.load(), 0.0);
        HeldQueries.clear();
        EnqueueRenderCommand("ReleaseSkyTimingSlots", [](FRHICommandListImmediate& Cmd) {
            Cmd.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
        });
        FlushRenderingCommands();

        // A nonuniform sky catches wrong face indices or cube orientation.
        const auto ConstantRequest=Request;
        Sky->SetRadianceColors({4,2,1},{1,1,1},{0.125f,0.25f,0.5f},{0,0,0});
        Light->Recapture();
        for(int Poll=0;Poll<200 && Request==ConstantRequest;++Poll)
        {
            Pump();
            if(Request==ConstantRequest) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_NE(Request,ConstantRequest);
        EnqueueRenderCommand("CheckMetalSkyOrientation",[&](FRHICommandListImmediate& Cmd) {
            ASSERT_TRUE(Scene.SkyLighting->Active);
            const std::array<FVector3f,2> Expected{FVector3f{4,2,1},FVector3f{0.125f,0.25f,0.5f}};
            for(uint32 Index=0;Index<2;++Index)
            {
                FByteBuffer Pixels;
                ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,Scene.SkyLighting->Active->Radiance,0,4+Index,Pixels));
                ASSERT_EQ(Pixels.size(),128u*128u*8u);
                for(uint32 Channel=0;Channel<3;++Channel)
                {
                    uint16 Bits;
                    std::memcpy(&Bits,Pixels.data()+(64*128+64)*8+Channel*2,2);
                    EXPECT_NEAR(glm::unpackHalf2x16(uint32(Bits)).x,Expected[Index][Channel],0.01f);
                }
            }
        });
        FlushRenderingCommands();

        // An ordinary HDR asset uses RGBA32F source sampling through the same path.
        const std::array<float,4> Pixel{0.25f,0.5f,1.0f,1.0f};
        FByteBuffer Bytes(8*4*sizeof(Pixel));
        for(size_t Offset=0;Offset<Bytes.size();Offset+=sizeof(Pixel))
            std::memcpy(Bytes.data()+Offset,Pixel.data(),sizeof(Pixel));
        auto Panorama=Image::FImage::TryCreate({.Width=8,.Height=4,
            .Format=Image::ERawImageFormat::RGBA32F,.GammaSpace=Image::EImageGammaSpace::Linear},std::move(Bytes));
        ASSERT_TRUE(Panorama);
        auto Source=PrepareTextureCubePanoramaSource(Panorama->GetView(),4,0);
        ASSERT_TRUE(Source) << Source.error();
        auto* Cube=NewObject<DTextureCube>(World,"SpecifiedSkyCube");
        Cube->SetSource(std::move(*Source));
        Cube->SetBuildSettings(ETextureCubeSourceLayout::EquirectangularPanorama,32,0,8,4,false,ETextureCubeOutput::HDR);
        Cube->BeginCachePlatformData();
        ASSERT_TRUE(Cube->FinishCachePlatformData());
        EXPECT_EQ(Cube->GetBuiltPixelFormat(), EPixelFormat::RGBA32_FLOAT);
        Cube->UpdateResource();
        FlushRenderingCommands();
        PumpGameThreadDeferredWork();
        EnqueueRenderCommand("InspectSpecifiedSkySource", [&](FRHICommandListImmediate&) {
            const auto* Texture=Cube->GetTextureReferenceRHI()->GetReferencedTexture_RenderThread();
            EXPECT_NE(Texture,nullptr);
            if(Texture) EXPECT_EQ(Texture->GetFormat(),EPixelFormat::RGBA32_FLOAT);
        });
        FlushRenderingCommands();
        Light->SetSource(ESkyLightSourceMode::SpecifiedCube, Cube);
        Ready = false;
        WaitForReady();
        if (Ready) CheckEnergy({0.25f,0.5f,1.0f}, false);

        Light->SetEnabled(false);
        Pump();
        EXPECT_FALSE(Ready);
        Pump();
        EnqueueRenderCommand("VerifyMetalSkyRetirement", [&](FRHICommandListImmediate& Cmd) {
            GDynamicRHI->RHIBlockUntilGPUIdle();
        });
        FlushRenderingCommands();
        Pump();
        EnqueueRenderCommand("CheckMetalSkyRetired", [&](FRHICommandListImmediate&) {
            EXPECT_TRUE(Scene.SkyLighting->Retiring.empty());
            EXPECT_FALSE(Scene.SkyLighting->InFlight);
        });
        FlushRenderingCommands();
        EXPECT_TRUE(World->SetCurrentLevel(nullptr));
        World->SetRenderScene(nullptr);
        World->Shutdown();
        RemoveFromRoot(World);
        MarkObjectHierarchyAsGarbage(World);
        CollectGarbage();
        FSceneInterfaceTestAccess::ReleaseScene(SceneOwner);
        FlushRenderingCommands();
        Lifecycle.Shutdown();
        FlushRenderingCommands();
        ShutdownRenderingThread();
        RHIExit();
    }
    ShutdownAssetCompilingManager();
    ShutdownTaskSystem();
}
