#include "Engine/FPrimitiveSceneProxy.h"
#include "Engine/LightSceneProxy.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "NativeTestSupport.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "StaticMesh/StaticMeshResources.h"

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

namespace
{
	struct FObservedLight
	{
		bool bPresent = false;
		Durin::FDirectionalLightSceneData Data;
	};

	auto ObserveLight(Durin::FScene& Scene) -> FObservedLight
	{
		auto Result = std::make_shared<FObservedLight>();
		struct FObserveLightCommand
		{
			static constexpr auto GetName() -> const char* { return "ObserveLight"; }
		};
		Durin::EnqueueRenderCommand<FObserveLightCommand>(
			[&Scene, Result](Durin::FRHICommandListImmediate&) {
				Result->bPresent = Scene.GetDirectionalLight(Result->Data);
			});
		Durin::FlushRenderingCommands();
		return *Result;
	}

	class FRenderingThreadScope final
	{
	public:
		FRenderingThreadScope()
		{
			if (!Durin::GIsGameThreadIdInitialized)
			{
				Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
				Durin::GIsGameThreadIdInitialized = true;
			}
			Durin::InitRenderingThread();
		}
		~FRenderingThreadScope() { Durin::ShutdownRenderingThread(); }
	};
}

TEST(FRendererSceneContractTests, PrimitiveMembershipOwnsClassificationBoundsAndFifoLifetime)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-1.0, -2.0, -3.0),
		Durin::FVector3(1.0, 2.0, 3.0));
	const Durin::FPrimitiveSceneId Id(41);
	const Durin::FMatrix InitialTransform = glm::translate(
		Durin::FMatrix(1.0), Durin::FVector3(10.0, 20.0, 30.0));

	Scene.AddOrReplacePrimitive(Id,
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
		InitialTransform);
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);
	ASSERT_EQ(Scene.GetStaticMeshSceneInfos().size(), 1u);
	EXPECT_TRUE(Scene.GetTextureCubePreviewSceneInfos().empty());
	const Durin::FPrimitiveSceneInfo* Info = Scene.GetStaticMeshSceneInfos().front();
	EXPECT_EQ(Info->GetId(), Id);
	EXPECT_TRUE(Info->GetLocalBounds().bIsValid);
	EXPECT_EQ(Info->GetWorldBounds().Min, Durin::FVector3(9.0, 18.0, 27.0));
	EXPECT_EQ(Info->GetWorldBounds().Max, Durin::FVector3(11.0, 22.0, 33.0));

	Scene.AddOrReplacePrimitive(Id, nullptr, Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);

	Scene.RemovePrimitive(Id);
	Scene.UpdatePrimitiveTransform(Id, Durin::FMatrix(2.0));
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPrimitiveSceneInfos().empty());

	Scene.AddOrReplacePrimitive(Id,
		std::make_unique<Durin::FTextureCubePreviewSceneProxy>(nullptr, nullptr),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Scene.GetTextureCubePreviewSceneInfos().size(), 1u);
	EXPECT_TRUE(Scene.GetStaticMeshSceneInfos().empty());

	Scene.Release();
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPrimitiveSceneInfos().empty());
}

TEST(FRendererSceneContractTests, DirectionalLightProxyOutlivesPublisherAndUsesFifoReplacement)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	const Durin::FLightSceneId Id(7);
	{
		Durin::FDirectionalLightSceneData Data;
		Data.Intensity = 2.0f;
		Scene.AddOrReplaceDirectionalLight(Id,
			std::make_unique<Durin::FDirectionalLightSceneProxy>(Data));
	}
	Durin::FlushRenderingCommands();
	FObservedLight Observed = ObserveLight(Scene);
	ASSERT_TRUE(Observed.bPresent);
	EXPECT_EQ(Observed.Data.Intensity, 2.0f);

	Durin::FDirectionalLightSceneData Replacement;
	Replacement.Intensity = 5.0f;
	Scene.AddOrReplaceDirectionalLight(Id,
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Replacement));
	Scene.RemoveDirectionalLight(Id);
	Durin::FlushRenderingCommands();
	EXPECT_FALSE(ObserveLight(Scene).bPresent);

	Scene.AddOrReplaceDirectionalLight(Id,
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Replacement));
	Durin::FlushRenderingCommands();
	Observed = ObserveLight(Scene);
	ASSERT_TRUE(Observed.bPresent);
	EXPECT_EQ(Observed.Data.Intensity, 5.0f);
	Scene.Release();
	Durin::FlushRenderingCommands();
}
