#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MaterialPSOPrecacheCollector.h"
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
    EnqueueRenderCommand("AddMeshBatchFiltering", [](FRHICommandListImmediate& Commands) {
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
            FGraphicsPipelineStateInitializer Initializer;
            const auto& Processor = FPassProcessorManager::GetMeshPassProcessor(Pass);
            const auto Preparation = Processor.BuildPSOInitializer(Batch, Empty, nullptr, Initializer);
            FPreparedStaticMeshPrimitive Primitive; FPreparedStaticMeshDraw Draw;
            FResolvedStaticMeshView::FMaterialUniform Uniform;
            const FMeshDrawCommandBuildInput Input{Commands, Primitive, Draw, {}, Uniform};
            FMeshDrawCommandBindingCache Cache;
            FDynamicMeshDrawCommandStorage Storage; FMeshDrawCommandHandle Published;
            FDynamicPassMeshDrawListContext Context(Storage, {Pass}, Published);
            const auto Result = Processor.AddMeshBatch(Batch, Input, Cache, Context);
            // Preparation and draw building share eligibility, with no RHI resources needed for exclusion.
            const auto Expected = Relevant ? EShaderBuildStatus::Error : EShaderBuildStatus::Canceled;
            EXPECT_EQ(Preparation.Status, Expected); EXPECT_EQ(Result.Status, Expected);
            EXPECT_EQ(Initializer.BoundShaders.VertexShader, nullptr); EXPECT_FALSE(Published);
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
        FCachedMeshDrawCommandStorage Persistent;
        FCachedPassMeshDrawListContext Cached(Persistent, {EMeshPipelinePass::Forward}, Published); const auto CachedResult = Processor.AddMeshBatch(Batch, Input, Cache, Cached);
        EXPECT_EQ(CachedResult.Status, EShaderBuildStatus::Error); EXPECT_EQ(Published.get(), Original);
        Pipeline->Pass = EMeshPipelinePass::Forward;
        FDynamicMeshDrawCommandStorage Frame; FMeshDrawCommandHandle DynamicReceipt;
        FDynamicPassMeshDrawListContext Dynamic(Frame, {EMeshPipelinePass::Forward}, DynamicReceipt); const auto DynamicResult = Processor.AddMeshBatch(Batch, Input, Cache, Dynamic);
        EXPECT_EQ(DynamicResult.Status, EShaderBuildStatus::Error); EXPECT_FALSE(DynamicReceipt); EXPECT_EQ(Frame.Num({EMeshPipelinePass::Forward}), 0u);
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

	auto GPUCommand = std::make_shared<RendererPrivate::FMeshDrawCommand>();
	GPUCommand->FragmentBindings = std::shared_ptr<const RendererPrivate::FPreparedSurfaceMaterialBindings>(
			reinterpret_cast<const RendererPrivate::FPreparedSurfaceMaterialBindings*>(
				uintptr_t{2}),
			[](const RendererPrivate::FPreparedSurfaceMaterialBindings*) {});
	auto Pipeline = std::make_shared<RendererPrivate::FMeshPassPipeline>();
	Pipeline->Pass = RendererPrivate::EMeshPipelinePass::GBuffer; Pipeline->bGPUCulling = true;
	GPUCommand->Pipeline = Pipeline; GPUCommand->bFinalized = true;
	Resolved.DrawCommandStorage = std::make_shared<RendererPrivate::FDynamicMeshDrawCommandStorage>();
	Resolved.Draws[0].Commands[Pipeline->GetPassKey()] = Resolved.DrawCommandStorage->Publish(Pipeline->GetPassKey(), std::move(*GPUCommand));
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

TEST(FRendererSceneContractTests, AdjacentBindingGroupsRebindOnAnyChangeFailureOrBoundary)
{
	using namespace Durin;
	FMeshDrawCommandStateCache Group;
	std::array<int, 6> Owners{};
	uint32 BindCalls = 0;
	auto Bind = [&](int Pipeline, int Vertex, int Fragment, bool bSuccess = true) {
		return Group.Apply(&Owners[Pipeline], &Owners[Vertex], Fragment < 0 ? nullptr : &Owners[Fragment], [&] {
			++BindCalls;
			return bSuccess;
		});
	};
	ASSERT_TRUE(Bind(0, 1, 2));
	ASSERT_TRUE(Bind(0, 1, 2));
	EXPECT_EQ(BindCalls, 1u);
	ASSERT_TRUE(Bind(0, 3, 2)); // vertex change must also rebind the complete fragment state
	ASSERT_TRUE(Bind(0, 3, 4));
	ASSERT_TRUE(Bind(5, 3, 4));
	EXPECT_EQ(BindCalls, 4u);
	EXPECT_FALSE(Bind(0, 1, 2, false));
	ASSERT_TRUE(Bind(5, 3, 4)); // failed partial binding invalidates even the preceding group
	EXPECT_EQ(BindCalls, 6u);
	Group = {}; // new pass or intervening non-mesh work
	ASSERT_TRUE(Bind(5, 3, 4));
	EXPECT_EQ(BindCalls, 7u);
	ASSERT_TRUE(Bind(0, 1, -1));
	ASSERT_TRUE(Bind(0, 1, -1)); // opaque shadow has no fragment resources
	EXPECT_EQ(BindCalls, 8u);
}

TEST(FRendererSceneContractTests, PreparedSurfaceBindingsClearPreviousPayloadOnFailure)
{
	using namespace Durin;
	using namespace Durin::RendererPrivate;
	FShaderReflectionData Reflection;
	Reflection.ResourceBindings = {{.SetIndex = 1, .BindingIndex = 2,
		.Type = ERHIBindingType::UniformBuffer}};
	const FCompiledSurfaceBindingLayout Layout(Reflection);
	FResolvedSurfaceMaterial Material;
	Material.bCompiledLayout = true;
	FPreparedSurfaceMaterialBindings Bindings;
	const auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Fragment, FXxHash128{}));
	const auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
		"PreparedSurface", 64, 16, EBufferUsageFlags::UniformBuffer));
	const FRHIUniformBufferRange Range{Buffer.GetReference(), 16, 32};
	ASSERT_TRUE(PrepareCompiledSurfaceMaterial(Shader.GetReference(), Layout, Material,
		Range, {}, {}, {}, {}, Bindings));
	ASSERT_EQ(Bindings.GetResources().size(), 1u);
	EXPECT_EQ(Bindings.GetResources()[0].Offset, 16u);
	EXPECT_EQ(Bindings.GetResources()[0].Resource->GetResourceType(), ERHIResourceType::BufferView);
	EXPECT_FALSE(PrepareCompiledSurfaceMaterial(Shader.GetReference(), Layout, Material,
		{Buffer.GetReference(), 48, 32}, {}, {}, {}, {}, Bindings));
	EXPECT_EQ(Bindings.GetShader(), nullptr);
	EXPECT_TRUE(Bindings.GetResources().empty());
	FRHICommandList Commands;
	const auto Uniform = Commands.CreateUniformBuffer({64}, ERHIBufferLifetimeUsage::MultiFrame, FByteBuffer(64));
	ASSERT_TRUE(Uniform);
	ASSERT_TRUE(PrepareCompiledSurfaceMaterial(Shader.GetReference(), Layout, Material,
		{Uniform.GetReference(), 16, 32}, {}, {}, {}, {}, Bindings));
	ASSERT_EQ(Bindings.GetResources().size(), 1u);
	EXPECT_EQ(Bindings.GetResources()[0].Resource, Uniform.GetReference());
	EXPECT_EQ(Bindings.GetResources()[0].Offset, 16u);
	EXPECT_EQ(Bindings.GetResources()[0].Size, 32u);
	EXPECT_FALSE(PrepareCompiledSurfaceMaterial(Shader.GetReference(), Layout, Material,
		{Uniform.GetReference(), 48, 32}, {}, {}, {}, {}, Bindings));
	EXPECT_EQ(Bindings.GetShader(), nullptr);
	EXPECT_TRUE(Bindings.GetResources().empty());
}

TEST(FRendererSceneContractTests, PlannedPSOsCoverDrawingStatesAndRequiredNormalizedRoutes)
{
    using namespace Durin;
    using namespace Durin::RendererPrivate;
    auto SameDescription = [](const auto& A, const auto& B) {
        return A.Topology == B.Topology && A.RenderTargets == B.RenderTargets
            && A.Rasterizer == B.Rasterizer && A.Depth == B.Depth && A.ColorBlends == B.ColorBlends;
    };
    for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
    for (const auto Shading : {EMaterialShadingModel::Lit, EMaterialShadingModel::Unlit})
    for (const auto Topology : {EGeometryTopology::TriangleList, EGeometryTopology::LineList})
    for (bool bSupportsGBuffer : {false, true})
    for (bool bOrientationChanging : {false, true})
    for (bool bTwoSided : {false, true})
    {
        SCOPED_TRACE(std::format("blend={} shading={} topology={} gbuffer={} orientation={} two-sided={}",
            uint8(Blend), uint8(Shading), uint8(Topology), bSupportsGBuffer, bOrientationChanging, bTwoSided));
        FMaterialPSOPrecacheParams Params;
        Params.Material.ShaderMap.BlendMode = Blend;
        Params.Material.ShaderMap.ShadingModel = Shading;
        Params.Material.bTwoSided = bTwoSided;
        Params.PrecachePSOParams.bOrientationChanging = bOrientationChanging;
        Params.PrecachePSOParams.Topology = Topology;
        const auto Plan = SelectMaterialPSOPrecachePasses(Params, bSupportsGBuffer);
        std::vector<EMeshPipelinePass> DrawRoutes{EMeshPipelinePass::Forward, EMeshPipelinePass::HitProxy};
        if (Blend == EMaterialBlendMode::Translucent) DrawRoutes.push_back(EMeshPipelinePass::SortedTranslucency);
        else {
            DrawRoutes.push_back(EMeshPipelinePass::ShadowDepth);
            DrawRoutes.push_back(Shading == EMaterialShadingModel::Lit && bSupportsGBuffer
                ? EMeshPipelinePass::GBuffer : EMeshPipelinePass::HybridRetainedForward);
        }
        for (const auto Route : DrawRoutes)
        for (uint8 Variant = 0; Variant < 8; ++Variant)
        {
            const FMeshPipelineRasterOptions DrawRaster{(Variant & 4) != 0, (Variant & 1) != 0,
                bOrientationChanging, (Variant & 2) != 0};
            const auto DrawDescription = DescribeMeshPipeline(Route, MakeMeshPipelineState(Params.Material, DrawRaster),
                Topology, Blend, DrawRaster.bReversedZ);
            bool bCovered = false, bRequiredCovered = false;
            for (const auto& Entry : Plan)
            {
                if (Entry.Pass != Route) continue;
                const auto PrecacheDescription = DescribeMeshPipeline(Entry.Pass, MakeMeshPipelineState(Params.Material, Entry.Raster),
                    Topology, Blend, Entry.Raster.bReversedZ);
                if (!SameDescription(DrawDescription, PrecacheDescription)) continue;
                bCovered = true;
                bRequiredCovered |= Entry.bRequired;
            }
            EXPECT_TRUE(bCovered);
            if (Route == EMeshPipelinePass::ShadowDepth
                || (Route != EMeshPipelinePass::HitProxy && !DrawRaster.bWireframe && !DrawRaster.bReversedZ))
                EXPECT_TRUE(bRequiredCovered);
        }
    }
}

TEST(FRendererSceneContractTests, MaterialPSOPassSelectionUsesParticipationPathsAndMaterialEligibility)
{
    using namespace Durin;
    using namespace Durin::RendererPrivate;
    FMaterialPSOPrecacheParams Params;
    Params.Material.ShaderMap.BlendMode = EMaterialBlendMode::Opaque;
    Params.Material.ShaderMap.ShadingModel = EMaterialShadingModel::Lit;
    auto Count = [](const auto& Plan, EMeshPipelinePass Pass) {
        return std::ranges::count_if(Plan, [Pass](const auto& Entry) { return Entry.Pass == Pass; });
    };
    auto Plan = SelectMaterialPSOPrecachePasses(Params, true);
    EXPECT_EQ(Plan.size(), 22u);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 8);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::HybridRetainedForward), 0);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::SortedTranslucency), 0);
    EXPECT_EQ(std::ranges::count_if(Plan, [](const auto& Entry) { return Entry.bRequired; }), 6);
    for (const auto& Entry : Plan)
        if (Entry.bRequired) {
            EXPECT_FALSE(Entry.Raster.bWireframe);
            EXPECT_FALSE(Entry.Raster.bReversedZ);
            EXPECT_NE(Entry.Pass, EMeshPipelinePass::HitProxy);
        }
    Params.PrecachePSOParams.Participation.bCastShadow = false;
    Plan = SelectMaterialPSOPrecachePasses(Params, true);
    EXPECT_EQ(Plan.size(), 20u);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::ShadowDepth), 0);
    Params.PrecachePSOParams.Participation = {true, false};
    Plan = SelectMaterialPSOPrecachePasses(Params, true);
    ASSERT_EQ(Plan.size(), 2u);
    for (const auto& Entry : Plan) EXPECT_EQ(Entry.Pass, EMeshPipelinePass::ShadowDepth);
    Params.Material.ShaderMap.BlendMode = EMaterialBlendMode::Translucent;
    EXPECT_TRUE(SelectMaterialPSOPrecachePasses(Params, true).empty());
    Params.PrecachePSOParams.Participation = {false, false};
    EXPECT_TRUE(SelectMaterialPSOPrecachePasses(Params, true).empty());
    Params.PrecachePSOParams.Participation = {};
    Plan = SelectMaterialPSOPrecachePasses(Params, true);
    EXPECT_EQ(Plan.size(), 20u);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::SortedTranslucency), 8);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::ShadowDepth), 0);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 0);
    Params.Material.ShaderMap.BlendMode = EMaterialBlendMode::Masked;
    Params.Material.ShaderMap.ShadingModel = EMaterialShadingModel::Unlit;
    Plan = SelectMaterialPSOPrecachePasses(Params, true);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::HybridRetainedForward), 8);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::ShadowDepth), 2);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 0);
    Params.Material.ShaderMap.ShadingModel = EMaterialShadingModel::Lit;
    Plan = SelectMaterialPSOPrecachePasses(Params, false);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::HybridRetainedForward), 8);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 0);
    FSceneRenderPreparationConfig Config;
    Config.Settings.bHybrid = Config.Settings.bHitProxy = Config.bSupportsWireframe = false;
    Params.PrecachePSOParams.bReversedZ = true;
    Plan = SelectMaterialPSOPrecachePasses(Params, true, Config);
    EXPECT_EQ(Plan.size(), 6u);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::Forward), 4);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::ShadowDepth), 2);
    for (const auto& Entry : Plan) {
        EXPECT_FALSE(Entry.Raster.bWireframe);
        if (Entry.bRequired) EXPECT_EQ(Entry.Raster.bReversedZ, Entry.Pass != EMeshPipelinePass::ShadowDepth);
    }
    Config = {};
    Config.Settings.bForward = false;
    Plan = SelectMaterialPSOPrecachePasses(Params, true, Config);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::Forward), 0);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 8);
}

namespace
{
    auto MakeStoredCommand(Durin::RendererPrivate::FMeshPassKey Pass, uint64 Batch, bool Cached)
        -> Durin::RendererPrivate::FMeshDrawCommand
    {
        using namespace Durin; using namespace RendererPrivate;
        FMeshDrawCommand Command; Command.bFinalized = true; Command.bCached = Cached; Command.BatchId = Batch;
        auto Pipeline = std::make_shared<FMeshPassPipeline>(); Pipeline->Pass = Pass.Pass; Pipeline->bGPUCulling = Pass.bGPUCulling;
        Command.Pipeline = std::move(Pipeline); Command.Description = std::make_shared<FMeshDrawDescription>();
        return Command;
    }
}
TEST(FRendererSceneContractTests, PassCommandListsRetainSnapshotsAndRejectCrossFrameHandles)
{
    using namespace Durin; using namespace RendererPrivate;
    const FMeshPassKey Forward{EMeshPipelinePass::Forward}, Direct{EMeshPipelinePass::GBuffer}, GPU{EMeshPipelinePass::GBuffer, true};
    std::shared_ptr<const FMeshDrawCommand> Retained;
    std::weak_ptr<const FMeshPassPipeline> Lifetime;
    {
        FDynamicMeshDrawCommandStorage Frame, Other;
        auto Publish = [&](FMeshPassKey Pass, uint64 Batch) {
            FMeshDrawCommandHandle Receipt; FDynamicPassMeshDrawListContext Context(Frame, Pass, Receipt);
            Context.AddCommand(MakeStoredCommand(Pass, Batch, false)); Context.FinalizeCommand(); return Receipt;
        };
        const auto A = Publish(Forward, 1), B = Publish(Direct, 2), C = Publish(GPU, 3);
        Retained = Frame.GetCommand(A); Lifetime = Retained->Pipeline;
        EXPECT_EQ(Frame.Num(Forward), 1u); EXPECT_EQ(Frame.Num(Direct), 1u); EXPECT_EQ(Frame.Num(GPU), 1u);
        EXPECT_FALSE(Other.GetCommand(A));
        const auto OldGPU = Frame.GetCommand(C);
        FMeshDrawCommandHandle Completed;
        FDynamicPassMeshDrawListContext Completion(Frame, GPU, Completed, C);
        Completion.AddCommand(MakeStoredCommand(GPU, 99, false)); Completion.FinalizeCommand();
        EXPECT_EQ(Completed, C); EXPECT_EQ(Frame.Num(GPU), 1u);
        EXPECT_EQ(Frame.GetCommand(C)->BatchId, 99u); EXPECT_EQ(OldGPU->BatchId, 3u);
        for (uint64 I = 0; I < 200; ++I) Publish(Forward, I + 100);
        EXPECT_EQ(Frame.GetCommand(A), Retained); EXPECT_EQ(Frame.GetCommand(B)->BatchId, 2u);
        FMeshDrawCommandHandle Unpublished;
        { FDynamicPassMeshDrawListContext Aborted(Frame, Forward, Unpublished); Aborted.AddCommand(MakeStoredCommand(Forward, 999, false)); }
        EXPECT_FALSE(Unpublished); EXPECT_EQ(Frame.Num(Forward), 201u);
    }
    EXPECT_FALSE(Lifetime.expired()); EXPECT_EQ(Retained->BatchId, 1u);
    Retained.reset(); EXPECT_TRUE(Lifetime.expired());
}
TEST(FRendererSceneContractTests, CachedPassStorageBoundsRetentionAndPreservesEvictedOwners)
{
    using namespace Durin; using namespace RendererPrivate;
    FCachedMeshDrawCommandStorage Storage;
    auto Publish = [&](FMeshPassKey Pass, uint64 Batch) {
        std::shared_ptr<const FMeshDrawCommand> Out; FCachedPassMeshDrawListContext Context(Storage, Pass, Out);
        Context.AddCommand(MakeStoredCommand(Pass, Batch, true)); Context.FinalizeCommand(); return Out;
    };
    const FMeshPassKey Forward{EMeshPipelinePass::Forward}, GPU{EMeshPipelinePass::GBuffer, true};
    auto Held = Publish(Forward, 1); const auto Key = Held->GetKey();
    EXPECT_EQ(Storage.Find(Forward, Key), Held); EXPECT_FALSE(Storage.Find(GPU, Key));
    std::weak_ptr<const FMeshPassPipeline> Lifetime = Held->Pipeline;
    for (uint64 I = 0; I < 2050; ++I) Publish(I % 2 ? Forward : GPU, I + 2);
    EXPECT_EQ(Storage.Num(), 2048u); EXPECT_FALSE(Storage.Find(Forward, Key));
    EXPECT_EQ(Held->BatchId, 1u); EXPECT_FALSE(Lifetime.expired());
    Storage.Reset(); EXPECT_EQ(Storage.Num(), 0u); EXPECT_FALSE(Lifetime.expired());
    Held.reset(); EXPECT_TRUE(Lifetime.expired());
}
