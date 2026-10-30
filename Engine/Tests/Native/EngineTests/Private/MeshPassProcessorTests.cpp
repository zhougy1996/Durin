#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshRendererShared.h"
#include "Materials/MaterialRenderProxy.h"

#include <gtest/gtest.h>

TEST(FRendererSceneContractTests, MeshPassProcessorsExposeUECollectorArgumentsAndSelectDrawShaders)
{
	using namespace Durin; using namespace RendererPrivate;
	using FCollect = void (IPSOCollector::*)(const FSceneRenderPreparationConfig&, const FMaterialResource&,
		const FPSOPrecacheVertexFactoryData&, const FPSOPrecacheParams&, FPSOPrecacheDataArray&);
	static_assert(std::is_same_v<decltype(&IPSOCollector::CollectPSOInitializers), FCollect>);
	static_assert(std::is_base_of_v<IPSOCollector, FMeshPassProcessor>);
	ASSERT_EQ(FPassProcessorManager::GetRegisteredPasses().size(), 6u);
	std::unordered_set<uint8> Registered;
	for (const auto Pass : FPassProcessorManager::GetRegisteredPasses())
	{
		const auto* Processor = &FPassProcessorManager::GetMeshPassProcessor(Pass);
		EXPECT_TRUE(Registered.insert(static_cast<uint8>(Processor->GetPass())).second);
		EXPECT_EQ(&FPassProcessorManager::GetMeshPassProcessor(Processor->GetPass()), Processor);
		const auto Instance = FPassProcessorManager::CreateMeshPassProcessor(Pass);
		ASSERT_TRUE(Instance);
		EXPECT_EQ(Instance->GetPass(), Processor->GetPass());
		EXPECT_NE(Processor->GetFragmentShader(EMaterialBlendMode::Opaque).Type, nullptr);
	}
	const auto& Shadow = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::ShadowDepth);
	EXPECT_EQ(Shadow.GetVertexFactoryPass(), MaterialMeshPassShadow);
	EXPECT_NE(Shadow.GetFragmentShader(EMaterialBlendMode::Opaque).Type, Shadow.GetFragmentShader(EMaterialBlendMode::Masked).Type);
	EXPECT_NE(Shadow.GetFragmentShader(EMaterialBlendMode::Masked).Type,
		FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::Forward).GetFragmentShader(EMaterialBlendMode::Masked).Type);
	EXPECT_EQ(FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).GetVertexFactoryPass(true), MaterialMeshPassGBufferGPUCulling);
}

TEST(FRendererSceneContractTests, MeshPassCollectorsCoverEffectiveDrawRasterStates)
{
	using namespace Durin; using namespace RendererPrivate;
	FSceneRenderPreparationConfig Config;
	for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
		for (const auto Pass : FPassProcessorManager::GetRegisteredPasses())
			for (uint8 Bits = 0; Bits < 16; ++Bits)
			{
				const auto* Processor = &FPassProcessorManager::GetMeshPassProcessor(Pass);
				FMaterialPlanningPassIdentity Material;
				Material.ShaderMap.BlendMode = Blend;
				FPSOPrecacheParams Params;
				Params.bWireframe = (Bits & 4) != 0;
				Params.bReversedZ = (Bits & 2) != 0;
				Params.bOrientationChanging = (Bits & 8) != 0;
				const auto Draw = Processor->DescribeMeshDraw(MakeMeshPipelineState(Material,
					{Params.bWireframe, (Bits & 1) != 0, Params.bOrientationChanging, Params.bReversedZ}), Params.Topology, Blend, Params.bReversedZ);
				const auto Variants = Processor->GetPSOPrecacheVariants(Config, Params);
				EXPECT_TRUE(std::ranges::any_of(Variants, [&](const auto& Variant) {
					const auto Collected = Processor->DescribeMeshDraw(MakeMeshPipelineState(Material, Variant.Raster),
						Params.Topology, Blend, Variant.Raster.bReversedZ);
					return Draw.RenderTargets == Collected.RenderTargets && Draw.Rasterizer == Collected.Rasterizer
						&& Draw.Depth == Collected.Depth && Draw.ColorBlends == Collected.ColorBlends;
				}));
			}
}

TEST(FRendererSceneContractTests, PassProcessorRegistrationRejectsDuplicatesWithoutRemovingTheOriginal)
{
	using namespace Durin::RendererPrivate;
	const auto Pass = EMeshPipelinePass::Forward;
	const auto* Original = &FPassProcessorManager::GetMeshPassProcessor(Pass);
	const auto UnexpectedCreate = +[](FMeshPSOCollectorContext) -> std::unique_ptr<FMeshPassProcessor> {
		ADD_FAILURE() << "A duplicate factory must not be invoked.";
		return {};
	};
	{
		FRegisterPassProcessorCreateFunction Duplicate(UnexpectedCreate, Pass);
		EXPECT_FALSE(Duplicate.IsRegistered());
		EXPECT_EQ(&FPassProcessorManager::GetMeshPassProcessor(Pass), Original);
	}
	EXPECT_TRUE(FPassProcessorManager::IsRegistered(Pass));
	EXPECT_EQ(&FPassProcessorManager::GetMeshPassProcessor(Pass), Original);
	const auto First = FPassProcessorManager::CreateMeshPassProcessor(Pass);
	const auto Second = FPassProcessorManager::CreateMeshPassProcessor(Pass);
	ASSERT_TRUE(First && Second);
	EXPECT_NE(First.get(), Second.get());
	EXPECT_EQ(First->GetPass(), Pass);
	EXPECT_EQ(Second->GetPass(), Pass);
}

TEST(FRendererSceneContractTests, PassProcessorRegistrationRejectsNullFactoriesAndInvalidPasses)
{
	using namespace Durin::RendererPrivate;
	FRegisterPassProcessorCreateFunction Null(nullptr, EMeshPipelinePass::Forward);
	EXPECT_FALSE(Null.IsRegistered());
	const auto Invalid = static_cast<EMeshPipelinePass>(255);
	const auto Create = +[](FMeshPSOCollectorContext) -> std::unique_ptr<FMeshPassProcessor> { return {}; };
	FRegisterPassProcessorCreateFunction Registration(Create, Invalid);
	EXPECT_FALSE(Registration.IsRegistered());
	EXPECT_FALSE(FPassProcessorManager::IsRegistered(Invalid));
	EXPECT_FALSE(FPassProcessorManager::CreateMeshPassProcessor(Invalid));
	EXPECT_EQ(FPassProcessorManager::GetRegisteredPasses().size(), 6u);
}

TEST(FRendererSceneContractTests, PassProcessorFactoriesCreateIndependentInstancesAcrossThreads)
{
	using namespace Durin::RendererPrivate;
	const auto Passes = FPassProcessorManager::GetRegisteredPasses();
	std::atomic<bool> Valid = true;
	std::array<std::jthread, 4> Workers;
	for (auto& Worker : Workers) Worker = std::jthread([&] {
		for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
			for (const auto Pass : Passes) {
				const auto Instance = FPassProcessorManager::CreateMeshPassProcessor(Pass);
				if (!Instance || Instance->GetPass() != Pass || FPassProcessorManager::GetMeshPassProcessor(Pass).GetPass() != Pass)
					Valid = false;
			}
	});
	for (auto& Worker : Workers) Worker.join();
	EXPECT_TRUE(Valid.load());
}
