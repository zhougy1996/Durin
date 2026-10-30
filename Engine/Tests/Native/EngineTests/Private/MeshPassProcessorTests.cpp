#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshRendererShared.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/StaticMeshBatchBinding.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RenderingThread.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>

namespace
{
    class FMeshPassRenderingThreadScope
    {
    public:
        FMeshPassRenderingThreadScope()
        {
            if (!Durin::GIsGameThreadIdInitialized) {
                Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
                Durin::GIsGameThreadIdInitialized = true;
            }
            if (!Durin::IsTaskSchedulerRunning()) require(Durin::InitializeTaskScheduler(2));
            Durin::InitRenderingThread();
        }
        ~FMeshPassRenderingThreadScope() { Durin::ShutdownRenderingThread(); }
    };
}

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

TEST(FRendererSceneContractTests, AddMeshBatchFiltersBeforeBuildingShadersOrCommands)
{
    using namespace Durin; using namespace RendererPrivate;
    FMeshPassRenderingThreadScope RenderingThread;
    EnqueueRenderCommand("AddMeshBatchFiltering", [](FRHICommandListImmediate&) {
        const auto Factory = FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
        ASSERT_TRUE(Factory);
        for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
        for (const auto Shading : {EMaterialShadingModel::Lit, EMaterialShadingModel::Unlit})
        for (bool Main : {false, true})
        for (bool Shadow : {false, true})
        for (bool GBuffer : {false, true})
        for (const auto Pass : FPassProcessorManager::GetRegisteredPasses()) {
            FMaterialPlanningPassIdentity Material;
            Material.ShaderMap.BlendMode = Blend; Material.ShaderMap.ShadingModel = Shading;
            const bool Translucent = Blend == EMaterialBlendMode::Translucent;
            const bool Deferred = !Translucent && Shading == EMaterialShadingModel::Lit && GBuffer;
            bool Relevant = Main;
            if (Pass == EMeshPipelinePass::ShadowDepth) Relevant = Shadow && !Translucent;
            else if (Pass == EMeshPipelinePass::GBuffer) Relevant = Main && Deferred;
            else if (Pass == EMeshPipelinePass::HybridRetainedForward) Relevant = Main && !Translucent && !Deferred;
            else if (Pass == EMeshPipelinePass::SortedTranslucency) Relevant = Main && Translucent;
            const FMeshPassProcessorBatch Batch{Material, *Factory, {}, EGeometryTopology::TriangleList, {Shadow, Main}, GBuffer};
            FMeshPassShaders Empty;
            FPSOPrecacheDataArray Initializers(1);
            FMeshPassDrawListContext Context(Empty, nullptr, Initializers, true, EPSOPrecachePriority::Medium);
            FPassProcessorManager::GetMeshPassProcessor(Pass).AddMeshBatch(Batch, Context);
            // Applicable elements reach context validation; excluded elements must
            // leave it untouched without requiring RHI, shaders or declarations.
            EXPECT_EQ(Context.GetResult().Status, Relevant ? EShaderBuildStatus::Error : EShaderBuildStatus::Canceled);
            EXPECT_EQ(Initializers.size(), 1u);
            EXPECT_EQ(Initializers.front().GraphicsInitializer.BoundShaders.VertexShader, nullptr);
        }
    });
    FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, RetainedMeshCommandsValidateInputsAndKeyPublishedResources)
{
    using namespace Durin; using namespace RendererPrivate;
    FMeshPassRenderingThreadScope RenderingThread;
    EnqueueRenderCommand("RetainedMeshCommandReview", [](FRHICommandListImmediate&) {
        auto Input = std::make_shared<FStaticMeshBatchBinding>();
        const auto Factory = FindMeshVertexFactory(Input->GetFactoryKey()); ASSERT_TRUE(Factory);
        FMaterialPlanningPassIdentity Material;
        auto Description = std::make_shared<FMeshDrawDescription>();
        Description->Material.PlanningPassIdentity = Material;
        Description->PipelineKey.Material = Material;
        Description->PipelineKey.FactoryKey = Input->GetFactoryKey();
        Description->PipelineKey.LayoutKey = Input->GetLayoutKey();
        FPreparedStaticMeshPrimitive Primitive;
        Primitive.PrimitiveId = FPrimitiveComponentId(17); Primitive.BatchId = 23;
        Primitive.CollectedBinding = Input;
        FPreparedStaticMeshDraw Draw; Draw.Command = Description; Draw.MaterialUniformIndex = 0;
        FResolvedStaticMeshView View; View.Draws.resize(1); View.Draws[0].bReady = true;
        View.PrimitiveUniforms.resize(1); View.MaterialUniforms.resize(1);
        auto ViewBindings = std::make_shared<FPreparedSurfaceMaterialBindings>();
        auto StableBindings = std::make_shared<FPreparedSurfaceMaterialBindings>();
        View.Draws[0].VertexPassBindings = ViewBindings;
        View.Draws[0].FragmentPassBindings = ViewBindings;
        View.Draws[0].SurfaceBindings = StableBindings;
        // Opaque pipeline identity only: no native object is dereferenced here.
        auto OptionalOwner = std::make_shared<int>(0);
        View.Draws[0].GBufferGPUCullingPipeline = std::shared_ptr<const FGBufferPipeline>(OptionalOwner,
            reinterpret_cast<const FGBufferPipeline*>(OptionalOwner.get()));
        const auto RequestedKey = FMeshDrawCommand::MakeKey(Description.get(), Primitive.PrimitiveId, Primitive.BatchId, View.Draws[0]);
        // Optional binding failure removes the GPU route before command publication.
        View.Draws[0].GBufferGPUCullingPipeline.reset();
        FMeshPassProcessorBatch Batch{Material, *Factory,
            {Description->PipelineKey.Rasterizer, Description->PipelineKey.Depth, Description->PipelineKey.ColorBlend},
            Description->PipelineKey.Topology, {}};
        const auto& Processor = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::Forward);
        std::shared_ptr<const FMeshDrawCommand> Published;
        FMeshPassDrawListContext Context(Primitive, Draw, View, Published);
        Processor.AddMeshBatch(Batch, Context);
        ASSERT_TRUE(Context.GetResult()); ASSERT_TRUE(Published);
        EXPECT_NE(Published->GetKey(), RequestedKey);
        EXPECT_EQ(Published->GetKey(), FMeshDrawCommand::MakeKey(Description.get(), Primitive.PrimitiveId, Primitive.BatchId, View.Draws[0]));
        EXPECT_EQ(Published->Record.SurfaceBindings, StableBindings);
        EXPECT_FALSE(Published->Record.VertexPassBindings); EXPECT_FALSE(Published->Record.FragmentPassBindings);
        const auto* Original = Published.get();
        auto ExpectRejected = [&] {
            FMeshPassDrawListContext Mismatch(Primitive, Draw, View, Published);
            Processor.AddMeshBatch(Batch, Mismatch);
            EXPECT_EQ(Mismatch.GetResult().Status, EShaderBuildStatus::Error);
            EXPECT_EQ(Published.get(), Original);
        };
        Material.ShaderMap.BlendMode = EMaterialBlendMode::Translucent; ExpectRejected(); Material = Description->Material.PlanningPassIdentity;
        const auto FactoryKey = Description->PipelineKey.FactoryKey;
        Description->PipelineKey.FactoryKey = {}; ExpectRejected(); Description->PipelineKey.FactoryKey = FactoryKey;
        const auto LayoutKey = Description->PipelineKey.LayoutKey;
        Description->PipelineKey.LayoutKey = {}; ExpectRejected(); Description->PipelineKey.LayoutKey = LayoutKey;
        Batch.Topology = EGeometryTopology::LineList; ExpectRejected(); Batch.Topology = Description->PipelineKey.Topology;
        Batch.PipelineState.Depth.bEnableWrite = !Batch.PipelineState.Depth.bEnableWrite; ExpectRejected();
        Batch.PipelineState.Depth = Description->PipelineKey.Depth;
        View.Draws[0].bReady = false; ExpectRejected();
        const auto PublishedKey = Published->GetKey(); Primitive.BatchId++;
        EXPECT_EQ(Published->GetKey(), PublishedKey);
        std::weak_ptr<const FMeshDrawDescription> DescriptionLifetime = Description;
        std::weak_ptr<const FVertexFactoryInputBinding> InputLifetime = Input;
        std::weak_ptr<const FPreparedSurfaceMaterialBindings> ViewLifetime = ViewBindings, StableLifetime = StableBindings;
        View = {}; Draw.Command.reset(); Description.reset(); Primitive.CollectedBinding.reset(); Input.reset();
        ViewBindings.reset(); StableBindings.reset();
        EXPECT_TRUE(ViewLifetime.expired());
        EXPECT_FALSE(DescriptionLifetime.expired()); EXPECT_FALSE(InputLifetime.expired()); EXPECT_FALSE(StableLifetime.expired());
        Published.reset();
        EXPECT_TRUE(DescriptionLifetime.expired()); EXPECT_TRUE(InputLifetime.expired()); EXPECT_TRUE(StableLifetime.expired());
    });
    FlushRenderingCommands();
}
