#include "Renderers/MeshPassProcessor.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/GBufferRendering.h"
#include "Renderers/MeshRendererShared.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/StaticMeshBatchBinding.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RenderingThread.h"
#include "SceneInfo.h"
#include "SceneTestAccess.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Threading/TaskRuntime.h"

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
	EXPECT_EQ(FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).GetVertexFactoryPass(), MaterialMeshPassGBuffer);
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
	const auto UnexpectedCreate = +[](int32) -> std::unique_ptr<FMeshPassProcessor> {
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
	const auto Create = +[](int32) -> std::unique_ptr<FMeshPassProcessor> { return {}; };
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
        auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
        std::shared_ptr<const FMeshDrawCommand> Published = std::make_shared<FMeshDrawCommand>();
        const auto* Original = Published.get();
        FCachedMeshDrawCommandStorage Persistent;
        FCachedPassMeshDrawListContext Cached(Persistent, {EMeshPipelinePass::Forward}, Published, Input); const auto CachedResult = Processor.AddMeshBatch(Batch, Input, Cache, Cached);
        EXPECT_EQ(CachedResult.Status, EShaderBuildStatus::Error); EXPECT_EQ(Published.get(), Original);
        Pipeline->Pass = EMeshPipelinePass::Forward;
        FDynamicMeshDrawCommandStorage Frame; FMeshDrawCommandHandle DynamicReceipt;
        FDynamicPassMeshDrawListContext Dynamic(Frame, {EMeshPipelinePass::Forward}, DynamicReceipt); const auto DynamicResult = Processor.AddMeshBatch(Batch, Input, Cache, Dynamic);
        EXPECT_EQ(DynamicResult.Status, EShaderBuildStatus::Error); EXPECT_FALSE(DynamicReceipt); EXPECT_EQ(Frame.Num({EMeshPipelinePass::Forward}), 0u);
    });
    FlushRenderingCommands();
}

namespace
{
    auto BuildTestInstanceCullingPlan(const Durin::FPreparedStaticMeshView& Prepared, const Durin::FResolvedStaticMeshView& Resolved,
        const Durin::FSceneView& View, bool Requested, bool Supported, size_t* Overflow = nullptr,
        size_t Budget = Durin::FInstanceCullingContext::MaximumGeneratedBytes,
        Durin::FInstanceCullingAdmissionDiagnostics* Diagnostics = nullptr) -> std::shared_ptr<Durin::FInstanceCullingContext>
    {
        auto Snapshot = std::make_shared<const Durin::FInstanceCullingView>(Durin::FInstanceCullingView{View.ViewLocation, {}});
        auto Context = std::make_shared<Durin::FInstanceCullingContext>(Snapshot, Durin::RendererPrivate::FMeshPassKey{Durin::RendererPrivate::EMeshPipelinePass::GBuffer});
        return Context->SetupDrawCommands(Durin::RendererPrivate::CollectMeshDrawCommands(Prepared, Resolved, {Durin::RendererPrivate::EMeshPipelinePass::GBuffer}, false), Requested, Supported, Overflow, Budget, Diagnostics) ? Context : nullptr;
    }
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
	const auto FragmentShader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Fragment, FXxHash128{}));
	GPUCommand->FragmentBindings.Shader = FragmentShader.GetReference();
	auto Pipeline = std::make_shared<RendererPrivate::FMeshPassPipeline>();
	Pipeline->Pass = RendererPrivate::EMeshPipelinePass::GBuffer;
	class FDeclaration final : public FRHIVertexDeclaration {
        FVertexDeclarationElementList Elements;
        auto GetElements() const -> const FVertexDeclarationElementList& override { return Elements; }
    };
    Pipeline->VertexDeclaration = new FDeclaration;
    const auto IndexBuffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("CullingIndices", 256, 4, EBufferUsageFlags::IndexBuffer));
    GPUCommand->IndexBuffer = IndexBuffer; GPUCommand->DrawParameters = Command->Geometry.GetIndexedDrawArguments();
    GPUCommand->Pipeline = Pipeline.get(); GPUCommand->bFinalized = true;
	Resolved.DrawCommandStorage = std::make_shared<RendererPrivate::FDynamicMeshDrawCommandStorage>();
	Resolved.Draws[0].Commands[Pipeline->GetPassKey()] = Resolved.DrawCommandStorage->Publish(Pipeline->GetPassKey(), std::move(*GPUCommand));
	Prepared.Primitives[0].CollectedBinding = std::make_shared<FStaticMeshBatchBinding>();

    auto SceneData = std::make_shared<FGPUSceneBufferSet>();
    SceneData->SceneData[1] = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("CullingSceneInstances",
        64 * sizeof(FInstanceSceneShaderData), 16, EBufferUsageFlags::StructuredBuffer));
    Resolved.GPUScenePrimitives.push_back({SceneData, 0, 0});
	FSceneView View;
	EXPECT_FALSE(BuildTestInstanceCullingPlan(
		Prepared, Resolved, View, true, true));
	for (size_t Index = 1;
		Index < FInstanceCullingContext::MinimumGroupCandidates; ++Index)
	{
		Prepared.Primitives.push_back(Prepared.Primitives[0]);
		Prepared.Primitives.back().WorldBounds = FBox(
			FVector3(static_cast<double>(Index * 4), -1.0, -1.0),
			FVector3(static_cast<double>(Index * 4 + 2), 1.0, 1.0));
		Prepared.Opaque.push_back({.Command = Command,
			.ResolvedIndex = static_cast<uint32>(Index),
			.PrimitiveIndex = static_cast<uint32>(Index)});
		Resolved.Draws.push_back(Resolved.Draws[0]);
        Resolved.GPUScenePrimitives.push_back({SceneData, static_cast<uint32>(Index), static_cast<uint32>(Index)});
	}
	EXPECT_FALSE(BuildTestInstanceCullingPlan(
		Prepared, Resolved, View, false, true));
	EXPECT_FALSE(BuildTestInstanceCullingPlan(
		Prepared, Resolved, View, true, false));
	const auto Plan = BuildTestInstanceCullingPlan(
		Prepared, Resolved, View, true, true);
	ASSERT_TRUE(Plan);
	ASSERT_EQ(Plan->Candidates.size(),
		FInstanceCullingContext::MinimumGroupCandidates);
	ASSERT_EQ(Plan->Arguments.size(), 1u);
	EXPECT_EQ(Plan->Arguments[0].IndexCount, 36u);
	EXPECT_EQ(Plan->Arguments[0].InstanceCount, 0u);
	EXPECT_EQ(Plan->Arguments[0].FirstIndex, 7u);
	EXPECT_EQ(Plan->Arguments[0].VertexOffset, -2);
	ASSERT_EQ(Plan->ArgumentByResolvedDraw.size(),
		FInstanceCullingContext::MinimumGroupCandidates);
	EXPECT_EQ(Plan->ArgumentByResolvedDraw.front(), 0u);
	EXPECT_TRUE(std::all_of(Plan->ArgumentByResolvedDraw.begin() + 1,
		Plan->ArgumentByResolvedDraw.end(), [](uint32 Value) {
			return Value == FInstanceCullingContext::GroupedMember;
		}));
    EXPECT_EQ(Plan->Candidates[0].InstanceId, 0u);
    EXPECT_EQ(Plan->Candidates[0].ArgumentIndex, 0u);
    EXPECT_EQ(Plan->Candidates.back().InstanceId, 63u);
    // Equivalent independent wrappers must group by effective state. Preserve
    // prepared order: an incompatible middle draw splits the run.
    {
        auto Visible = RendererPrivate::CollectMeshDrawCommands(Prepared, Resolved, Pipeline->GetPassKey(), false);
        std::vector<RendererPrivate::FMeshDrawCommand> Commands(Visible.size(), *Visible.front().Command);
        std::vector<RendererPrivate::FMeshPassPipeline> Pipelines(Visible.size(), *Pipeline);
        std::vector<RendererPrivate::FMeshPassBindings> Bindings(Visible.size(), *Visible.front().Pass);
        for (size_t Index = 0; Index < Visible.size(); ++Index) {
            Pipelines[Index].VertexDeclaration = new FDeclaration;
            Commands[Index].Pipeline = &Pipelines[Index]; Visible[Index].Command = &Commands[Index];
            Visible[Index].Pass = &Bindings[Index];
            Visible[Index].SceneData.Buffers = std::make_shared<FGPUSceneBufferSet>(*Visible[Index].SceneData.Buffers);
        }
        FInstanceCullingManager Manager;
        auto Context = Manager.CreateContext(Manager.RegisterView(View), Pipeline->GetPassKey());
        EXPECT_TRUE(Context->SetupDrawCommands(Visible, true, true));
        EXPECT_EQ(Context->Arguments.size(), 1u); EXPECT_EQ(Context->Candidates.size(), 64u);
        Visible[32].DepthBias[0] = 1.f;
        EXPECT_FALSE(Context->SetupDrawCommands(Visible, true, true)); EXPECT_TRUE(Context->Candidates.empty());
        Visible[32].DepthBias[0] = 0.f; Bindings[32].Fragment.Shader = FragmentShader;
        EXPECT_FALSE(Context->SetupDrawCommands(Visible, true, true));
        Bindings[32].Fragment.Shader = nullptr;
        Visible.push_back(Visible.back()); Visible.back().bSupportsInstanceCulling = false;
        EXPECT_TRUE(Context->SetupDrawCommands(Visible, true, true));
        FResolvedStaticMeshView Partition;
        Partition.DynamicInstancingGroups.push_back({Pipeline->GetPassKey(), {0, 64}, true});
        Context->bDispatched = true; Context->PrepareForSubmission(Partition);
        EXPECT_FALSE(Partition.DynamicInstancingGroups[0].bEnabled);
        Context->bDispatched = false; Context->PrepareForSubmission(Partition);
        EXPECT_TRUE(Partition.DynamicInstancingGroups[0].bEnabled);
    }
    constexpr size_t PerCandidateBytes = sizeof(FInstanceCullingCandidate) + sizeof(uint32);
	const size_t ExactBudget = Plan->Candidates.size() * PerCandidateBytes + sizeof(FRHIDrawIndexedIndirectArguments);
	size_t Overflow = 0;
	FInstanceCullingAdmissionDiagnostics Admission;
	EXPECT_TRUE(BuildTestInstanceCullingPlan(Prepared, Resolved, View, true, true, &Overflow, ExactBudget, &Admission));
	EXPECT_EQ(Overflow, 0u);
	EXPECT_EQ(Admission.BelowThresholdCandidates, 0u);
	EXPECT_FALSE(BuildTestInstanceCullingPlan(Prepared, Resolved, View, true, true, &Overflow, ExactBudget - 1));
	EXPECT_EQ(Overflow, 64u);
	EXPECT_FALSE(BuildTestInstanceCullingPlan(Prepared, Resolved, View, true, true, &Overflow, 0));
	EXPECT_EQ(Overflow, 64u);
	{
		auto ManyPrepared = Prepared;
		auto ManyResolved = Resolved;
		const size_t MaximumGroups = std::min(FInstanceCullingContext::MaximumGeneratedBytes / ExactBudget, FInstanceCullingContext::MaximumCandidates / 64);
		for (size_t Group = 1; Group <= MaximumGroups; ++Group)
		{
			auto GroupCommand = std::make_shared<FMeshDrawDescription>(*Command);
            auto GroupDraw = *Resolved.DrawCommandStorage->GetCommand(Resolved.Draws[0].Commands.at(Pipeline->GetPassKey()));
            std::get<FRHIDrawIndexedArguments>(GroupDraw.DrawParameters).FirstIndex += static_cast<uint32>(Group);
            const auto GroupReceipt = Resolved.DrawCommandStorage->Publish(Pipeline->GetPassKey(), std::move(GroupDraw));
			for (uint32 Index = 0; Index < 64; ++Index)
			{
				auto Draw = Prepared.Opaque[Index];
				Draw.Command = GroupCommand;
				Draw.ResolvedIndex = static_cast<uint32>(ManyResolved.Draws.size());
				ManyPrepared.Opaque.push_back(Draw);
				ManyResolved.Draws.push_back(Resolved.Draws[Index]);
                ManyResolved.Draws.back().Commands[Pipeline->GetPassKey()] = GroupReceipt;
			}
		}
		const auto ManyPlan = BuildTestInstanceCullingPlan(ManyPrepared, ManyResolved, View, true, true, &Overflow);
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
		EXPECT_FALSE(BuildTestInstanceCullingPlan(Small, Resolved, View, true, true, nullptr,
			FInstanceCullingContext::MaximumGeneratedBytes, &Admission));
		EXPECT_EQ(Admission.BelowThresholdCandidates, 63u);
		Small.Opaque.clear();
		EXPECT_FALSE(BuildTestInstanceCullingPlan(Small, Resolved, View, true, true));
	}

    {
        auto Invalid = Resolved;
        Invalid.GPUScenePrimitives[0].InstanceId = 64;
        EXPECT_FALSE(BuildTestInstanceCullingPlan(Prepared, Invalid, View, true, true, nullptr,
            FInstanceCullingContext::MaximumGeneratedBytes, &Admission));
        EXPECT_EQ(Admission.PreparationUnavailableCandidates, 1u);
        EXPECT_EQ(Admission.BelowThresholdCandidates, 63u);
        auto InstanceInput = std::make_shared<FStaticMeshBatchBinding>();
        InstanceInput->DeclarationElements[0].InputRate = FRHIVertexElementIdentity::EInputRate::Instance;
        auto Unsupported = Prepared;
        for (auto& Primitive : Unsupported.Primitives) Primitive.CollectedBinding = InstanceInput;
        EXPECT_FALSE(BuildTestInstanceCullingPlan(Unsupported, Resolved, View, true, true, nullptr,
            FInstanceCullingContext::MaximumGeneratedBytes, &Admission));
        EXPECT_EQ(Admission.IncompatibleCandidates, 64u);
    }

	for (auto& Primitive : Prepared.Primitives)
		Primitive.VertexDomain = EVertexDeformationDomain::Spline;
	EXPECT_FALSE(BuildTestInstanceCullingPlan(
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
	const auto TranslatedPlan = BuildTestInstanceCullingPlan(Prepared, Resolved, View, true, true);
	ASSERT_TRUE(TranslatedPlan);
	ASSERT_EQ(TranslatedPlan->Candidates.size(), Plan->Candidates.size());
	for (size_t Index = 0; Index < Plan->Candidates.size(); ++Index)
	{
		EXPECT_EQ(TranslatedPlan->Candidates[Index].InstanceId, Plan->Candidates[Index].InstanceId);
	}

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
	FMeshDrawShaderBindings Bindings;
	const auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Fragment, FXxHash128{}));
	const auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
		"PreparedSurface", 64, 16, EBufferUsageFlags::UniformBuffer));
	const FRHIUniformBufferRange Range{Buffer.GetReference(), 16, 32};
	ASSERT_TRUE(PrepareCompiledSurfaceMaterial(Shader.GetReference(), Layout, Material,
		Range, {}, {}, {}, {}, Bindings));
	ASSERT_EQ(Bindings.GetResources().size(), 1u);
	EXPECT_EQ(Bindings.GetResources()[0].Offset, 16u);
	EXPECT_EQ(Bindings.GetResources()[0].Resource, Buffer.GetReference());
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

namespace
{
    // Each test owns the data borrowed by its commands; no process-wide fixture state.
    struct FStoredCommandSource
    {
        Durin::RendererPrivate::FMeshPassPipeline Pipeline;
        Durin::FMeshDrawDescription Description;
        explicit FStoredCommandSource(Durin::RendererPrivate::FMeshPassKey Pass)
        { Pipeline.Pass = Pass.Pass; }
        auto Key(uint64 Batch) const -> Durin::RendererPrivate::FCachedMeshDrawCommandStorage::FKey
        { return Durin::RendererPrivate::FCachedMeshDrawCommandStorage::MakeKey(&Description, Durin::InvalidPrimitiveComponentId, Batch, Pipeline); }
        auto MakeCommand(uint64, bool Cached) const -> Durin::RendererPrivate::FMeshDrawCommand
        {
            using namespace Durin; using namespace RendererPrivate;
            FMeshDrawCommand Command; Command.bFinalized = true; Command.bCached = Cached;
            Command.Pipeline = &Pipeline;
            return Command;
        }
    };
}
TEST(FRendererSceneContractTests, PassCommandListsRejectCrossFrameHandles)
{
    using namespace Durin; using namespace RendererPrivate;
    const FMeshPassKey Pass{EMeshPipelinePass::Forward};
    FStoredCommandSource DrawSource(Pass);
    FDynamicMeshDrawCommandStorage Frame, Other;
    const auto Receipt = Frame.Publish(Pass, DrawSource.MakeCommand(1, false));
    ASSERT_TRUE(Frame.GetCommand(Receipt)); EXPECT_FALSE(Other.GetCommand(Receipt));
    const auto OldFrame = GRenderFrameCounterRenderThread;
    ++GRenderFrameCounterRenderThread;
    EXPECT_FALSE(Frame.GetCommand(Receipt));
    GRenderFrameCounterRenderThread = OldFrame;
}
TEST(FRendererSceneContractTests, CachedPassStorageRetainsLiveCommandsUntilSourceInvalidation)
{
    using namespace Durin; using namespace RendererPrivate;
    const FMeshPassKey Pass{EMeshPipelinePass::Forward};
    FStoredCommandSource DrawSource(Pass);
    FCachedMeshDrawCommandStorage Storage;
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    const auto Held = Storage.Publish(DrawSource.Key(1), DrawSource.MakeCommand(1, true), *Source);
    const auto Key = DrawSource.Key(1);
    for (uint64 I = 0; I < 2050; ++I) Storage.Publish(DrawSource.Key(I + 2), DrawSource.MakeCommand(I + 2, true), *Source);
    EXPECT_EQ(Storage.Num(), 2051u); EXPECT_TRUE(Storage.Find(Key, *Source));
    Source->RemoveCachedMeshDrawCommands();
    EXPECT_EQ(Storage.Num(), 0u); EXPECT_FALSE(Storage.Find(Key, *Source));
    EXPECT_TRUE(Held->bFinalized);
}
TEST(FRendererSceneContractTests, VisibleCommandMemoryDoesNotKeepSourceResourcesAlive)
{
    using namespace Durin; using namespace RendererPrivate;
    FCachedMeshDrawCommandStorage Cache;
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    const FMeshPassKey Pass{EMeshPipelinePass::Forward};
    FStoredCommandSource DrawSource(Pass);
    auto Command = Cache.Publish(DrawSource.Key(41), DrawSource.MakeCommand(41, true), *Source);
    auto Bindings = std::make_shared<FMeshPassBindings>();
    FDynamicMeshDrawCommandStorage Submission;
    const auto Receipt = Submission.PublishVisible(Command, Cache.FindInfo(DrawSource.Key(41), *Source), Bindings, {1, 2, 3});
    const auto* Borrowed = Submission.GetVisible(Receipt); ASSERT_TRUE(Borrowed);
    std::weak_ptr<FPrimitiveSceneInfo> Life = Source;
    Cache.Reset(); Command.reset(); Source.reset();
    EXPECT_TRUE(Life.expired());
    EXPECT_FALSE(Submission.GetVisible(Receipt));
    EXPECT_TRUE(Borrowed->Command->bFinalized);
}
TEST(FRendererSceneContractTests, CachedCommandCannotReviveAReleasedSource)
{
    using namespace Durin; using namespace RendererPrivate;
    FCachedMeshDrawCommandStorage Cache;
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    const FMeshPassKey Pass{EMeshPipelinePass::Forward};
    FStoredCommandSource DrawSource(Pass);
    const auto Command = Cache.Publish(DrawSource.Key(13), DrawSource.MakeCommand(13, true), *Source);
    const auto Key = DrawSource.Key(13);
    ASSERT_TRUE(Cache.Find(Key, *Source));
    Source->RemoveCachedMeshDrawCommands();
    EXPECT_EQ(Cache.Num(), 0u); EXPECT_FALSE(Cache.Find(Key, *Source));
}
TEST(FRendererSceneContractTests, ResourceSlotEvictionDoesNotInvalidateUnrelatedSceneCommands)
{
    using namespace Durin; using namespace RendererPrivate;
    FCachedMeshDrawCommandStorage Cache;
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    const FMeshPassKey Pass{EMeshPipelinePass::Forward};
    FStoredCommandSource DrawSource(Pass);
    TRendererResourceSlotCache<int, int> Slots;
    Slots.FindOrAddBounded(1, 1);
    const auto Key = DrawSource.Key(14);
    const auto Descriptor = Cache.Publish(Key, DrawSource.MakeCommand(14, true), *Source);
    ASSERT_TRUE(Cache.Find(Key, *Source));
    Slots.FindOrAddBounded(2, 1);
    EXPECT_EQ(Cache.Num(), 1u); EXPECT_TRUE(Cache.Find(Key, *Source));
    Source->RemoveCachedMeshDrawCommands();
    EXPECT_EQ(Cache.Num(), 0u);
    EXPECT_TRUE(Descriptor->bFinalized);
}

TEST(FRendererSceneContractTests, RemovingASourceUniformSlotInvalidatesItsCachedCommand)
{
    using namespace Durin; using namespace RendererPrivate;
    FMeshPassRenderingThreadScope RenderingThread;
    EnqueueRenderCommand("RemoveSourceUniformSlot", [](FRHICommandListImmediate& Commands) {
        auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
        auto& Uniform = Source->GetPrimitiveUniformBuffer(45);
        ASSERT_TRUE(Uniform.Update(Commands, FByteBuffer(16)));
        const TRefCountPtr<FRHIResource> ObservedBuffer(Uniform.GetRange().Buffer);
        const auto InitialReferences = ObservedBuffer->GetRefCount();
        const FMeshPassKey Pass{EMeshPipelinePass::Forward};
        FStoredCommandSource DrawSource(Pass);
        auto Draw = DrawSource.MakeCommand(45, true);
        Draw.VertexBindings.Parameters.push_back({Uniform.GetRange().Buffer});
        const auto Key = DrawSource.Key(45);
        FCachedMeshDrawCommandStorage Cache;
        const auto Cached = Cache.Publish(Key, std::move(Draw), *Source);
        ASSERT_TRUE(Cache.Find(Key, *Source));
        EXPECT_EQ(ObservedBuffer->GetRefCount(), InitialReferences);
        Source->RetainPrimitiveUniformBuffers({});
        EXPECT_EQ(ObservedBuffer->GetRefCount(), InitialReferences - 1);
        EXPECT_EQ(Cache.Num(), 0u); EXPECT_FALSE(Cache.Find(Key, *Source));
    });
    FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, ShaderBindingsRecordOnlyChangedSlotsAndRespectCacheReset)
{
    using namespace Durin; using namespace RendererPrivate;
    FRHICommandList Commands;
    Commands.SwitchPipeline(ERHIPipeline::Graphics);
    const auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Fragment, FXxHash128{}));
    const auto A = Commands.CreateUniformBuffer({32}, ERHIBufferLifetimeUsage::MultiFrame, FByteBuffer(32));
    const auto B = Commands.CreateUniformBuffer({32}, ERHIBufferLifetimeUsage::MultiFrame, FByteBuffer(32));
    FMeshDrawShaderBindings Material, Pass;
    Material.Shader = Pass.Shader = Shader.GetReference();
    Material.Parameters = {{A.GetReference(), 1, 0, 0, ERHIBindingType::UniformBuffer, 0, 32}};
    Pass.Parameters = {{B.GetReference(), 0, 0, 0, ERHIBindingType::UniformBuffer, 0, 32}};
    FShaderBindingState State;
    const auto Before = Commands.GetNumRecordedCommands();
    ASSERT_TRUE(Material.SetOnCommandList(Commands, State));
    ASSERT_TRUE(Pass.SetOnCommandList(Commands, State));
    EXPECT_EQ(Commands.GetNumRecordedCommands(), Before + 2);
    Material.SetOnCommandList(Commands, State); Pass.SetOnCommandList(Commands, State);
    EXPECT_EQ(Commands.GetNumRecordedCommands(), Before + 2);
    Material.Parameters[0].Resource = B.GetReference();
    Material.SetOnCommandList(Commands, State); Pass.SetOnCommandList(Commands, State);
    EXPECT_EQ(Commands.GetNumRecordedCommands(), Before + 3);
    State = {};
    Material.SetOnCommandList(Commands, State); Pass.SetOnCommandList(Commands, State);
    EXPECT_EQ(Commands.GetNumRecordedCommands(), Before + 5);
}

TEST(FRendererSceneContractTests, InstanceBindingOverridesPreserveSourcesAndRestoreCachedRanges)
{
    using namespace Durin; using namespace RendererPrivate;
    FRHICommandList Commands; Commands.SwitchPipeline(ERHIPipeline::Graphics);
    const auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Vertex, FXxHash128{}));
    const auto A = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("OriginalIds", 64, 4, EBufferUsageFlags::StructuredBuffer));
    const auto B = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("OverrideIds", 96, 4, EBufferUsageFlags::StructuredBuffer));
    FMeshDrawShaderBindings Bindings; Bindings.Shader = Shader;
    Bindings.Parameters = {{A.GetReference(), 0, 31, 0, ERHIBindingType::StorageBuffer, 0, 64}};
    FShaderBindingState State;
    const auto Before = Commands.GetNumRecordedCommands();
    ASSERT_TRUE(Bindings.SetOnCommandList(Commands, State));
    ASSERT_TRUE(Bindings.SetOnCommandList(Commands, State, B, 16));
    ASSERT_EQ(State.Parameters.size(), 1u);
    EXPECT_EQ(State.Parameters[0].Resource, B.GetReference());
    EXPECT_EQ(State.Parameters[0].Offset, 16u); EXPECT_EQ(State.Parameters[0].Size, 80u);
    EXPECT_EQ(Bindings.Parameters[0].Resource, A.GetReference());
    EXPECT_EQ(Bindings.Parameters[0].Offset, 0u); EXPECT_EQ(Bindings.Parameters[0].Size, 64u);
    Bindings.SetOnCommandList(Commands, State, B, 16);
    EXPECT_EQ(Commands.GetNumRecordedCommands(), Before + 2);
    EXPECT_FALSE(Bindings.SetOnCommandList(Commands, State, B, 97));
    EXPECT_FALSE(Bindings.SetOnCommandList(Commands, State, B, 1));
    ASSERT_TRUE(Bindings.SetOnCommandList(Commands, State));
    EXPECT_EQ(Commands.GetNumRecordedCommands(), Before + 3);
    EXPECT_EQ(State.Parameters[0].Resource, A.GetReference());
    EXPECT_EQ(State.Parameters[0].Offset, 0u); EXPECT_EQ(State.Parameters[0].Size, 64u);
}

TEST(FRendererSceneContractTests, SourceInvalidationRetiresOnlyItsPreparedHandles)
{
    using namespace Durin; using namespace RendererPrivate;
    FMeshPassRenderingThreadScope RenderingThread;
    EnqueueRenderCommand("UniformBindingInvalidation", [](FRHICommandListImmediate& Commands) {
        auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
        auto Other = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
        auto& Uniform = Source->GetPrimitiveUniformBuffer(1);
        ASSERT_TRUE(Uniform.Update(Commands, FByteBuffer(16)));
        const FMeshPassKey Pass{EMeshPipelinePass::Forward};
        FStoredCommandSource DrawSource(Pass);
        FCachedMeshDrawCommandStorage Cache;
        auto Command = DrawSource.MakeCommand(1, true);
        Command.VertexBindings.Parameters.push_back({Uniform.GetRange().Buffer});
        Cache.Publish(DrawSource.Key(1), std::move(Command), *Source);
        Cache.Publish(DrawSource.Key(2), DrawSource.MakeCommand(2, true), *Other);
        FDynamicMeshDrawCommandStorage Frame;
        const auto Receipt = Frame.PublishVisible(Cache.Find(DrawSource.Key(1), *Source), Cache.FindInfo(DrawSource.Key(1), *Source), std::make_shared<FMeshPassBindings>(), {});
        const auto* Original = Uniform.GetRange().Buffer;
        ASSERT_TRUE(Uniform.Update(Commands, FByteBuffer(16, std::byte{1})));
        EXPECT_EQ(Uniform.GetRange().Buffer, Original);
        EXPECT_EQ(Cache.Num(), 2u); EXPECT_TRUE(Frame.GetVisible(Receipt));
        Source->RemoveCachedMeshDrawCommands();
        EXPECT_FALSE(Cache.Find(DrawSource.Key(1), *Source));
        EXPECT_TRUE(Cache.Find(DrawSource.Key(2), *Other)); EXPECT_FALSE(Frame.GetVisible(Receipt));
        Cache.Publish(DrawSource.Key(1), DrawSource.MakeCommand(1, true), *Source);
        Source->RemoveCachedMeshDrawCommands();
        ASSERT_TRUE(Uniform.Update(Commands, FByteBuffer(32)));
        EXPECT_NE(Uniform.GetRange().Buffer, Original);
        EXPECT_EQ(Cache.Num(), 1u); EXPECT_TRUE(Cache.Find(DrawSource.Key(2), *Other));
    });
    FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, ReplacingDrawBindingsReusesTheSceneCacheLocation)
{
    using namespace Durin; using namespace RendererPrivate;
    FStoredCommandSource DrawSource({EMeshPipelinePass::Forward});
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    FCachedMeshDrawCommandStorage Cache;
    const auto Original = DrawSource.Key(1);
    Cache.Publish(Original, DrawSource.MakeCommand(1, true), *Source);
    auto Replacement = Original;
    ++Replacement.Bindings[2];
    Cache.Publish(Replacement, DrawSource.MakeCommand(1, true), *Source);
    EXPECT_EQ(Cache.Num(), 1u);
    EXPECT_FALSE(Cache.Find(Original, *Source)); EXPECT_TRUE(Cache.Find(Replacement, *Source));
}

TEST(FRendererSceneContractTests, SceneDrawSlotsAreIsolatedAndRetiredSlotsCannotReviveVisibleCommands)
{
    using namespace Durin; using namespace RendererPrivate;
    FStoredCommandSource DrawSource({EMeshPipelinePass::Forward});
    auto A = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    auto B = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    FCachedMeshDrawCommandStorage SceneA, SceneB;
    const auto Key = DrawSource.Key(1);
    auto First = SceneA.Publish(Key, DrawSource.MakeCommand(1, true), *A);
    auto Second = SceneB.Publish(Key, DrawSource.MakeCommand(1, true), *B);
    FDynamicMeshDrawCommandStorage Frame;
    const auto Old = Frame.PublishVisible(First, SceneA.FindInfo(Key, *A), std::make_shared<FMeshPassBindings>(), {});
    const auto Other = Frame.PublishVisible(Second, SceneB.FindInfo(Key, *B), std::make_shared<FMeshPassBindings>(), {});
    A->RemoveCachedMeshDrawCommands();
    EXPECT_FALSE(Frame.GetVisible(Old)); EXPECT_TRUE(Frame.GetVisible(Other));
    SceneA.Publish(Key, DrawSource.MakeCommand(1, true), *A);
    EXPECT_FALSE(Frame.GetVisible(Old)); EXPECT_TRUE(SceneA.Find(Key, *A));
    EXPECT_TRUE(SceneB.Find(Key, *B));
}
TEST(FRendererSceneContractTests, StructuralMaterialPublicationInvalidatesOnlyPrimitivesUsingThatProxy)
{
    using namespace Durin; using namespace RendererPrivate;
    FMeshPassRenderingThreadScope RenderingThread;
    FSceneTestOwner Scene;
    FMaterialRenderProxyRef A(new FMaterialRenderProxy), B(new FMaterialRenderProxy);
    FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId(9101),
        std::make_unique<FStaticMeshSceneProxy>(nullptr, std::vector{A}), FMatrix(1));
    FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId(9102),
        std::make_unique<FStaticMeshSceneProxy>(nullptr, std::vector{B}), FMatrix(1));
    FlushRenderingCommands();
    EnqueueRenderCommand("MaterialSceneInvalidation", [&](FRHICommandListImmediate&) {
        Scene->CachedDrawLists = std::make_shared<FCachedMeshDrawCommandStorage>();
        const auto& Primitives = Scene->GetPrimitiveSceneInfos(); ASSERT_EQ(Primitives.size(), 2u);
        FStoredCommandSource DrawSource({EMeshPipelinePass::Forward});
        const auto Key = DrawSource.Key(1);
        const auto First = Scene->CachedDrawLists->Publish(Key, DrawSource.MakeCommand(1, true), *Primitives[0]);
        const auto Other = Scene->CachedDrawLists->Publish(Key, DrawSource.MakeCommand(1, true), *Primitives[1]);
        const auto FirstInfo = Scene->CachedDrawLists->FindInfo(Key, *Primitives[0]);
        const auto OtherInfo = Scene->CachedDrawLists->FindInfo(Key, *Primitives[1]);
        FMaterialResource Publication;
        Publication.StaticProperties = FMaterialStaticProperties{};
        const auto Revision = Scene->GetHitProxyRevision_RenderThread();
        A->ApplyPublication_RenderThread(Publication);
        EXPECT_GT(Scene->GetHitProxyRevision_RenderThread(), Revision);
        EXPECT_EQ(FirstInfo->CommandIndex, INDEX_NONE); EXPECT_NE(OtherInfo->CommandIndex, INDEX_NONE);
        EXPECT_EQ(Scene->CachedDrawLists->Num(), 1u);
        Scene->UpdateStaticMeshes();
        const auto Rebuilt = Scene->CachedDrawLists->Publish(Key, DrawSource.MakeCommand(1, true), *Primitives[0]);
        const auto RebuiltInfo = Scene->CachedDrawLists->FindInfo(Key, *Primitives[0]);
        FMaterialLocalRenderParameter Scalar; Scalar.Id = FGuid::NewGuid(); Scalar.Value = 0.5f;
        Publication.Parameters.push_back(Scalar);
        A->ApplyPublication_RenderThread(std::move(Publication));
        EXPECT_NE(RebuiltInfo->CommandIndex, INDEX_NONE); EXPECT_NE(OtherInfo->CommandIndex, INDEX_NONE);
        A->ReleaseRHI();
        EXPECT_EQ(RebuiltInfo->CommandIndex, INDEX_NONE); EXPECT_NE(OtherInfo->CommandIndex, INDEX_NONE);
        Scene->CachedDrawLists->Reset();
    });
    FlushRenderingCommands();
    Scene.Reset();
    FlushRenderingCommands();
    A = nullptr; B = nullptr;
    FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, PrimitiveSourceKeepsItsPipelineAliveWhileCachedCommandsOwnOnlyMemory)
{
    using namespace Durin; using namespace RendererPrivate;
    FStoredCommandSource DrawSource({EMeshPipelinePass::Forward});
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    auto Pipeline = std::make_shared<FMeshPassPipeline>();
    const std::weak_ptr<const FMeshPassPipeline> Lifetime = Pipeline;
    FCachedMeshDrawCommandStorage Scene;
    auto Draw = DrawSource.MakeCommand(1, true); Draw.Pipeline = Pipeline.get();
    const auto Key = FCachedMeshDrawCommandStorage::MakeKey(&DrawSource.Description, InvalidPrimitiveComponentId, 1, *Pipeline);
    const auto Entry = Scene.Publish(Key, std::move(Draw), *Source, nullptr, {}, Pipeline);
    const auto Info = Scene.FindInfo(Key, *Source);
    Pipeline.reset();
    EXPECT_FALSE(Lifetime.expired()); EXPECT_NE(Info->CommandIndex, INDEX_NONE);
    Source->RemoveCachedMeshDrawCommands();
    EXPECT_TRUE(Lifetime.expired()); EXPECT_EQ(Info->CommandIndex, INDEX_NONE);
    EXPECT_TRUE(Entry->bFinalized);
}
