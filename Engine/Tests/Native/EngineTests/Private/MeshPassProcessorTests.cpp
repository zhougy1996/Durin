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
	ASSERT_EQ(GetMeshPassProcessors().size(), 6u);
	std::unordered_set<uint8> Registered;
	for (const auto* Processor : GetMeshPassProcessors())
	{
		EXPECT_TRUE(Registered.insert(static_cast<uint8>(Processor->GetPass())).second);
		EXPECT_EQ(&GetMeshPassProcessor(Processor->GetPass()), Processor);
		const auto Instance = Processor->CreatePSOCollector({});
		ASSERT_TRUE(Instance);
		EXPECT_EQ(Instance->GetPass(), Processor->GetPass());
		EXPECT_NE(Processor->GetFragmentShader(EMaterialBlendMode::Opaque).Type, nullptr);
	}
	const auto& Shadow = GetMeshPassProcessor(EMeshPipelinePass::ShadowDepth);
	EXPECT_EQ(Shadow.GetVertexFactoryPass(), MaterialMeshPassShadow);
	EXPECT_NE(Shadow.GetFragmentShader(EMaterialBlendMode::Opaque).Type, Shadow.GetFragmentShader(EMaterialBlendMode::Masked).Type);
	EXPECT_NE(Shadow.GetFragmentShader(EMaterialBlendMode::Masked).Type,
		GetMeshPassProcessor(EMeshPipelinePass::Forward).GetFragmentShader(EMaterialBlendMode::Masked).Type);
	EXPECT_EQ(GetMeshPassProcessor(EMeshPipelinePass::GBuffer).GetVertexFactoryPass(true), MaterialMeshPassGBufferGPUCulling);
}

TEST(FRendererSceneContractTests, MeshPassCollectorsCoverEffectiveDrawRasterStates)
{
	using namespace Durin; using namespace RendererPrivate;
	FSceneRenderPreparationConfig Config;
	for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
		for (const auto* Processor : GetMeshPassProcessors())
			for (uint8 Bits = 0; Bits < 16; ++Bits)
			{
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
