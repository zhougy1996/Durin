#include "Renderers/MeshPassProcessor.h"
#include "Renderers/GBufferRendering.h"
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

TEST(FRendererSceneContractTests, PerPassMeshDrawCommandsOwnIndependentPipelineAndMaterialState)
{
    using namespace Durin; using namespace RendererPrivate;
    auto Description = std::make_shared<FMeshDrawDescription>();
    auto Input = std::make_shared<FStaticMeshBatchBinding>();
    auto Forward = std::make_shared<FMeshPassPipeline>(); Forward->Pass = EMeshPipelinePass::Forward;
    auto GBuffer = std::make_shared<FMeshPassPipeline>(); GBuffer->Pass = EMeshPipelinePass::GBuffer;
    auto GPU = std::make_shared<FMeshPassPipeline>(*GBuffer); GPU->bGPUCulling = true;
    auto MakeCommand = [&](const std::shared_ptr<const FMeshPassPipeline>& Pipeline) {
        auto Command = std::make_shared<FMeshDrawCommand>();
        Command->Description = Description; Command->Input = Input; Command->Pipeline = Pipeline;
        Command->PrimitiveId = FPrimitiveComponentId(17); Command->BatchId = 23; Command->bCached = true;
        return Command;
    };
    auto A = MakeCommand(Forward), B = MakeCommand(GBuffer), C = MakeCommand(GPU);
    EXPECT_NE(A->GetKey(), B->GetKey()); EXPECT_NE(B->GetKey(), C->GetKey());
    const auto StableKey = B->GetKey(); C.reset(); GPU.reset();
    EXPECT_EQ(B->GetKey(), StableKey);
    EXPECT_FALSE(A->VertexPassBindings); EXPECT_FALSE(A->FragmentPassBindings);
    std::weak_ptr<const FMeshDrawDescription> DescriptionLifetime = Description;
    std::weak_ptr<const FVertexFactoryInputBinding> InputLifetime = Input;
    std::weak_ptr<const FMeshPassPipeline> PipelineLifetime = Forward;
    Description.reset(); Input.reset(); Forward.reset(); GBuffer.reset();
    EXPECT_FALSE(DescriptionLifetime.expired()); EXPECT_FALSE(InputLifetime.expired()); EXPECT_FALSE(PipelineLifetime.expired());
    A.reset(); EXPECT_TRUE(PipelineLifetime.expired()); EXPECT_FALSE(DescriptionLifetime.expired());
    B.reset(); EXPECT_TRUE(DescriptionLifetime.expired()); EXPECT_TRUE(InputLifetime.expired());
}

TEST(FRendererSceneContractTests, CachedAndDynamicMeshContextsRejectUnpreparedPassesWithoutPublishing)
{
    using namespace Durin; using namespace RendererPrivate;
    FMeshPassRenderingThreadScope RenderingThread;
    EnqueueRenderCommand("PerPassMeshCommandContexts", [](FRHICommandListImmediate& Commands) {
        auto Binding = std::make_shared<FStaticMeshBatchBinding>();
        const auto Factory = FindMeshVertexFactory(Binding->GetFactoryKey()); ASSERT_TRUE(Factory);
        auto Description = std::make_shared<FMeshDrawDescription>();
        Description->PipelineKey.Material = Description->Material.PlanningPassIdentity;
        Description->PipelineKey.FactoryKey = Binding->GetFactoryKey(); Description->PipelineKey.LayoutKey = Binding->GetLayoutKey();
        FPreparedStaticMeshPrimitive Primitive; Primitive.CollectedBinding = Binding;
        FPreparedStaticMeshDraw Draw; Draw.Command = Description; Draw.MaterialUniformIndex = 0;
        auto Pipeline = std::make_shared<FMeshPassPipeline>(); Pipeline->Pass = EMeshPipelinePass::GBuffer;
        FResolvedStaticMeshView::FMaterialUniform Material;
        FMeshDrawCommandBuildInput Input{Commands, Primitive, Draw, Pipeline, Material};
        FMeshDrawCommandBindingCache Cache;
        const FMeshPassProcessorBatch Batch{Description->Material.PlanningPassIdentity, *Factory,
            {Description->PipelineKey.Rasterizer, Description->PipelineKey.Depth, Description->PipelineKey.ColorBlend}, Description->PipelineKey.Topology, {}};
        const auto& Processor = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::Forward);
        std::shared_ptr<const FMeshDrawCommand> Published = std::make_shared<FMeshDrawCommand>();
        const auto* Original = Published.get();
        FCachedPassMeshDrawListContext Cached(Input, Cache, Published); Processor.AddMeshBatch(Batch, Cached);
        EXPECT_EQ(Cached.GetResult().Status, EShaderBuildStatus::Error); EXPECT_EQ(Published.get(), Original);
        Pipeline->Pass = EMeshPipelinePass::Forward;
        FDynamicPassMeshDrawListContext Dynamic(Input, Cache, Published); Processor.AddMeshBatch(Batch, Dynamic);
        EXPECT_EQ(Dynamic.GetResult().Status, EShaderBuildStatus::Error); EXPECT_EQ(Published.get(), Original);
    });
    FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, GBufferGPUCullingPlanIsOptInAndPreservesIndexedArguments)
{
	using namespace Durin;
	FPreparedStaticMeshView Prepared;
	Prepared.Primitives.push_back({
		.VertexDomain = EVertexDeformationDomain::Local,
		.WorldBounds = FBox(FVector3(-2.0), FVector3(2.0)),
		.BoundsCenter = FVector3(3.0, 4.0, 5.0)});
	Prepared.Primitives[0].WorldToLocal[0][0] = 2.0f;
	auto Command = std::make_shared<FMeshDrawDescription>();
	Command->bSupportsGBuffer = true;
	Command->Geometry = {.bIndexed = true, .ElementCount = 36,
		.FirstElement = 7, .VertexOffset = -2};
	Prepared.Opaque.push_back({.Command = Command, .ResolvedIndex = 0,
		.PrimitiveIndex = 0});
	FResolvedStaticMeshView Resolved;
	Resolved.Draws.resize(1);
	Resolved.Draws[0].bReady = true;
	Resolved.Draws[0].GBufferGPUCullingPipeline =
		std::shared_ptr<const FGBufferPipeline>(
			reinterpret_cast<const FGBufferPipeline*>(uintptr_t{1}),
			[](const FGBufferPipeline*) {});
	auto GPUCommand = std::make_shared<RendererPrivate::FMeshDrawCommand>();
	GPUCommand->FragmentBindings = std::shared_ptr<const RendererPrivate::FPreparedSurfaceMaterialBindings>(
			reinterpret_cast<const RendererPrivate::FPreparedSurfaceMaterialBindings*>(
				uintptr_t{2}),
			[](const RendererPrivate::FPreparedSurfaceMaterialBindings*) {});
	Resolved.Draws[0].Commands[3] = std::move(GPUCommand);
	Prepared.Primitives[0].CollectedBinding =
		std::shared_ptr<const FVertexFactoryInputBinding>(
			reinterpret_cast<const FVertexFactoryInputBinding*>(uintptr_t{3}),
			[](const FVertexFactoryInputBinding*) {});
	FSceneView View;
	EXPECT_FALSE(BuildGBufferGPUCullingPlan(
		Prepared, Resolved, View, true, true));
	for (size_t Index = 1;
		Index < FGBufferGPUCullingPlan::MinimumGroupCandidates; ++Index)
	{
		Prepared.Primitives.push_back(Prepared.Primitives[0]);
		Prepared.Primitives.back().WorldBounds = FBox(
			FVector3(static_cast<double>(Index * 4), -1.0, -1.0),
			FVector3(static_cast<double>(Index * 4 + 2), 1.0, 1.0));
		Prepared.Opaque.push_back({.Command = Command,
			.ResolvedIndex = static_cast<uint32>(Index),
			.PrimitiveIndex = static_cast<uint32>(Index)});
		Resolved.Draws.push_back(Resolved.Draws[0]);
	}
	EXPECT_FALSE(BuildGBufferGPUCullingPlan(
		Prepared, Resolved, View, false, true));
	EXPECT_FALSE(BuildGBufferGPUCullingPlan(
		Prepared, Resolved, View, true, false));
	const auto Plan = BuildGBufferGPUCullingPlan(
		Prepared, Resolved, View, true, true);
	ASSERT_TRUE(Plan);
	ASSERT_EQ(Plan->Candidates.size(),
		FGBufferGPUCullingPlan::MinimumGroupCandidates);
	ASSERT_EQ(Plan->Arguments.size(), 1u);
	ASSERT_EQ(Plan->Transforms.size(),
		FGBufferGPUCullingPlan::MinimumGroupCandidates);
	EXPECT_EQ(Plan->Arguments[0].IndexCount, 36u);
	EXPECT_EQ(Plan->Arguments[0].InstanceCount, 0u);
	EXPECT_EQ(Plan->Arguments[0].FirstIndex, 7u);
	EXPECT_EQ(Plan->Arguments[0].VertexOffset, -2);
	ASSERT_EQ(Plan->ArgumentByResolvedDraw.size(),
		FGBufferGPUCullingPlan::MinimumGroupCandidates);
	EXPECT_EQ(Plan->ArgumentByResolvedDraw.front(), 0u);
	EXPECT_TRUE(std::all_of(Plan->ArgumentByResolvedDraw.begin() + 1,
		Plan->ArgumentByResolvedDraw.end(), [](uint32 Value) {
			return Value == FGBufferGPUCullingPlan::GroupedMember;
		}));
	EXPECT_FLOAT_EQ(Plan->Candidates[0].BoundsMin.x, -2.0f);
	EXPECT_EQ(Plan->Candidates[0].ArgumentIndex, 0u);
	EXPECT_FLOAT_EQ(Plan->Transforms[0].WorldToLocal[0][0], 2.0f);
	EXPECT_FLOAT_EQ(Plan->Transforms[0].BoundsCenter.x, 3.0f);
	EXPECT_FLOAT_EQ(Plan->Transforms[0].BoundsCenter.y, 4.0f);
	EXPECT_FLOAT_EQ(Plan->Transforms[0].BoundsCenter.z, 5.0f);
	EXPECT_FLOAT_EQ(Plan->Transforms[0].BoundsCenter.w, 1.0f);

	constexpr size_t PerCandidateBytes = sizeof(FGBufferGPUCullingCandidate) + sizeof(uint32)
		+ sizeof(RendererPrivate::FStaticMeshTransformUniform);
	const size_t ExactBudget = Plan->Candidates.size() * PerCandidateBytes + sizeof(FRHIDrawIndexedIndirectArguments);
	size_t Overflow = 0;
	FGPUCullingAdmissionDiagnostics Admission;
	EXPECT_TRUE(BuildGBufferGPUCullingPlan(Prepared, Resolved, View, true, true, &Overflow, ExactBudget, &Admission));
	EXPECT_EQ(Overflow, 0u);
	EXPECT_EQ(Admission.BelowThresholdCandidates, 0u);
	EXPECT_FALSE(BuildGBufferGPUCullingPlan(Prepared, Resolved, View, true, true, &Overflow, ExactBudget - 1));
	EXPECT_EQ(Overflow, 64u);
	EXPECT_FALSE(BuildGBufferGPUCullingPlan(Prepared, Resolved, View, true, true, &Overflow, 0));
	EXPECT_EQ(Overflow, 64u);
	{
		auto ManyPrepared = Prepared;
		auto ManyResolved = Resolved;
		const size_t MaximumGroups = FGBufferGPUCullingPlan::MaximumGeneratedBytes / ExactBudget;
		for (size_t Group = 1; Group <= MaximumGroups; ++Group)
		{
			auto GroupCommand = std::make_shared<FMeshDrawDescription>(*Command);
			for (uint32 Index = 0; Index < 64; ++Index)
			{
				auto Draw = Prepared.Opaque[Index];
				Draw.Command = GroupCommand;
				Draw.ResolvedIndex = static_cast<uint32>(ManyResolved.Draws.size());
				ManyPrepared.Opaque.push_back(Draw);
				ManyResolved.Draws.push_back(Resolved.Draws[Index]);
			}
		}
		const auto ManyPlan = BuildGBufferGPUCullingPlan(ManyPrepared, ManyResolved, View, true, true, &Overflow);
		ASSERT_TRUE(ManyPlan);
		ASSERT_EQ(ManyPlan->Arguments.size(), MaximumGroups);
		EXPECT_EQ(Overflow, 64u);
		for (size_t Group = 0; Group < MaximumGroups; ++Group)
		{
			EXPECT_EQ(ManyPlan->Arguments[Group].FirstInstance, Group * 64);
			EXPECT_EQ(ManyPlan->Candidates[Group * 64].ArgumentIndex, Group);
		}
	}
	{
		auto Small = Prepared;
		Small.Opaque.pop_back();
		EXPECT_FALSE(BuildGBufferGPUCullingPlan(Small, Resolved, View, true, true, nullptr,
			FGBufferGPUCullingPlan::MaximumGeneratedBytes, &Admission));
		EXPECT_EQ(Admission.BelowThresholdCandidates, 63u);
		Small.Opaque.clear();
		EXPECT_FALSE(BuildGBufferGPUCullingPlan(Small, Resolved, View, true, true));
	}

	for (auto& Primitive : Prepared.Primitives)
		Primitive.VertexDomain = EVertexDeformationDomain::Spline;
	EXPECT_FALSE(BuildGBufferGPUCullingPlan(
		Prepared, Resolved, View, true, true));
	// Translating the whole scene by a huge origin must not collapse GPU bounds.
	View.ViewLocation = FVector3{1099511627776.0, -1099511627776.0, 1099511627776.0};
	View.ViewMatrix = Math::TranslationMatrix(-View.ViewLocation);
	for (auto& Primitive : Prepared.Primitives)
	{
		Primitive.VertexDomain = EVertexDeformationDomain::Local;
		Primitive.WorldBounds.Min += View.ViewLocation;
		Primitive.WorldBounds.Max += View.ViewLocation;
		Primitive.BoundsCenter += View.ViewLocation;
		Primitive.LocalToWorld = Math::TranslationMatrix(View.ViewLocation) * Primitive.LocalToWorld;
		Primitive.WorldToLocal = Primitive.WorldToLocal * Math::TranslationMatrix(-View.ViewLocation);
	}
	const auto TranslatedPlan = BuildGBufferGPUCullingPlan(Prepared, Resolved, View, true, true);
	ASSERT_TRUE(TranslatedPlan);
	ASSERT_EQ(TranslatedPlan->Candidates.size(), Plan->Candidates.size());
	for (size_t Index = 0; Index < Plan->Candidates.size(); ++Index)
	{
		EXPECT_EQ(TranslatedPlan->Candidates[Index].BoundsMin, Plan->Candidates[Index].BoundsMin);
		EXPECT_EQ(TranslatedPlan->Candidates[Index].BoundsMax, Plan->Candidates[Index].BoundsMax);
		EXPECT_EQ(TranslatedPlan->Transforms[Index].LocalToWorld, Plan->Transforms[Index].LocalToWorld);
	}

}
