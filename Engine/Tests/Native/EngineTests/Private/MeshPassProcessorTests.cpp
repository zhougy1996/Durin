#include "MeshDrawCommandTestSupport.h"
#include "MaterialRenderProxyUpdateTestSupport.h"
#include "Renderers/MeshPassProcessor.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/GBufferRendering.h"
#include "Renderers/MeshRendererShared.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialBindingSchema.h"
#include "Rendering/StaticMeshBatchBinding.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RenderingThread.h"
#include "SceneInfo.h"
#include "SceneTestAccess.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Threading/TaskRuntime.h"

#include <gtest/gtest.h>

namespace Durin
{
    struct FInstanceCullingContextTestAccess
    {
        static auto SetDispatched(FInstanceCullingContext& Context, bool Value) -> void { Context.bDispatched = Value; }
    };
}

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
        ~FMeshPassRenderingThreadScope()
        {
            Durin::ShutdownRenderingThread();
        }
    };
}

TEST(FRendererSceneContractTests, MeshPassProcessorsSelectPassShaders)
{
	using namespace Durin; using namespace RendererPrivate;
	std::unordered_set<uint8> Registered;
	for (const auto Pass : FPassProcessorManager::GetRegisteredPasses())
	{
		const auto* Processor = &FPassProcessorManager::GetMeshPassProcessor(Pass);
		EXPECT_TRUE(Registered.insert(static_cast<uint8>(Processor->GetPass())).second);
		EXPECT_EQ(&FPassProcessorManager::GetMeshPassProcessor(Processor->GetPass()), Processor);
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

TEST(FRendererSceneContractTests, PassProcessorRegistrationRejectsInvalidFactoriesWithoutReplacingLivePasses)
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

	FRegisterPassProcessorCreateFunction Null(nullptr, EMeshPipelinePass::Forward);
	EXPECT_FALSE(Null.IsRegistered());
	const auto Invalid = static_cast<EMeshPipelinePass>(255);
	const auto Create = +[](int32) -> std::unique_ptr<FMeshPassProcessor> { return {}; };
	FRegisterPassProcessorCreateFunction Registration(Create, Invalid);
	EXPECT_FALSE(Registration.IsRegistered());
	EXPECT_FALSE(FPassProcessorManager::IsRegistered(Invalid));
	EXPECT_FALSE(FPassProcessorManager::CreateMeshPassProcessor(Invalid));
}

TEST(FRendererSceneContractTests, AddMeshBatchFiltersBeforeBuildingShadersOrCommands)
{
    using namespace Durin; using namespace RendererPrivate;
    FMeshPassRenderingThreadScope RenderingThread;
    EnqueueRenderCommand("AddMeshBatchFiltering", [](FRHICommandListImmediate& Commands) {
        const auto Factory = FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
        ASSERT_TRUE(Factory);
        for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
        for (const auto Shading : {EMaterialShadingModel::DefaultLit, EMaterialShadingModel::Unlit})
        for (bool Main : {false, true})
        for (bool Shadow : {false, true})
        for (bool GBuffer : {false, true})
        for (const auto Pass : FPassProcessorManager::GetRegisteredPasses()) {
            FMaterialPlanningPassIdentity Material;
            Material.ShaderMap.BlendMode = Blend; Material.ShaderMap.ShadingModel = Shading;
            const bool Translucent = Blend == EMaterialBlendMode::Translucent;
            const bool Deferred = !Translucent && Shading == EMaterialShadingModel::DefaultLit && GBuffer;
            bool Relevant = Main;
            if (Pass == EMeshPipelinePass::ShadowDepth) Relevant = Shadow && !Translucent;
            else if (Pass == EMeshPipelinePass::GBuffer) Relevant = Main && Deferred;
            else if (Pass == EMeshPipelinePass::HybridRetainedForward) Relevant = Main && !Translucent && !Deferred;
            else if (Pass == EMeshPipelinePass::SortedTranslucency) Relevant = Main && Translucent;
            const FMeshPassProcessorBatch Batch{Material, *Factory, {}, EGeometryTopology::TriangleList, {Shadow, Main}, GBuffer};
            FMeshPassShaders Empty;
            FGraphicsPipelineStateInitializer Initializer;
            auto ProcessorInstance = FPassProcessorManager::CreateMeshPassProcessor(Pass); auto& Processor = *ProcessorInstance;
            const auto Preparation = Processor.BuildPSOInitializer(Batch, Empty, nullptr, Initializer);
            FPreparedStaticMeshPrimitive Primitive; FPreparedStaticMeshDraw Draw;
            FResolvedStaticMeshView::FMaterialUniform Uniform;
            const FMeshDrawCommandBuildInput Input{Commands, Primitive, Draw, {}, Uniform};
            FMeshDrawCommandBindingCache Cache;
            FDynamicMeshDrawCommandStorage Storage; FParallelMeshDrawCommandPass Published({Pass});
            FDynamicPassMeshDrawListContext Context(Storage, Published, Cache);
            Processor.SetDrawListContext(&Context);
            const auto Result = Processor.AddMeshBatch(Batch, Input);
            // Preparation and draw building share eligibility, with no RHI resources needed for exclusion.
            const auto Expected = Relevant ? EShaderBuildStatus::Error : EShaderBuildStatus::Canceled;
            EXPECT_EQ(Preparation.Status, Expected); EXPECT_EQ(Result.Status, Expected);
            EXPECT_EQ(Initializer.BoundShaders.VertexShader, nullptr); EXPECT_TRUE(Published.GetMeshDrawCommands().empty());
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
        auto Pipeline = std::make_shared<const FMeshPassPipeline>(FMeshPassPipelineInitializer{.Pass = EMeshPipelinePass::GBuffer});
        FResolvedStaticMeshView::FMaterialUniform Material;
        FMeshDrawCommandBuildInput Input{Commands, Primitive, Draw, Pipeline, Material};
        FMeshDrawCommandBindingCache Cache;
        const FMeshPassProcessorBatch Batch{Description->Material.PlanningPassIdentity, *Factory,
            {Description->PipelineKey.Rasterizer, Description->PipelineKey.Depth, Description->PipelineKey.ColorBlend}, Description->PipelineKey.Topology, {}};
        auto ProcessorInstance = FPassProcessorManager::CreateMeshPassProcessor(EMeshPipelinePass::Forward); auto& Processor = *ProcessorInstance;
        FCachedMeshDrawCommandStorage Persistent;
        FCachedPassMeshDrawListContext Cached(Persistent, {EMeshPipelinePass::Forward}, Cache);
        Processor.SetDrawListContext(&Cached);
        const auto CachedResult = Processor.AddMeshBatch(Batch, Input);
        EXPECT_EQ(CachedResult.Status, EShaderBuildStatus::Error); EXPECT_EQ(Persistent.Num(), 0u);
        Input.Pipeline = std::make_shared<const FMeshPassPipeline>(FMeshPassPipelineInitializer{.Pass = EMeshPipelinePass::Forward});
        FDynamicMeshDrawCommandStorage Frame;
        FParallelMeshDrawCommandPass Pass({EMeshPipelinePass::Forward});
        FDynamicPassMeshDrawListContext Dynamic(Frame, Pass, Cache);
        Processor.SetDrawListContext(&Dynamic);
        // Reuse one destination across failed batches; neither failure publishes a command.
        for (uint32 Index = 0; Index < 2; ++Index) {
            Draw.ResolvedIndex = Index;
            EXPECT_EQ(Processor.AddMeshBatch(Batch, Input).Status, EShaderBuildStatus::Error);
            EXPECT_TRUE(Pass.GetMeshDrawCommands().empty());
        }
        Processor.SetDrawListContext(nullptr);
        EXPECT_NE(Processor.AddMeshBatch(Batch, Input).Error.find("local draw-list context"), std::string::npos);
        const auto& Prototype = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::Forward);
        EXPECT_NE(Prototype.AddMeshBatch(Batch, Input).Error.find("local draw-list context"), std::string::npos);
    });
    FlushRenderingCommands();
}

namespace
{
    auto PrepareTestMeshDrawCommands(const Durin::FPreparedStaticMeshView& Prepared, const Durin::FResolvedStaticMeshView& Source)
        -> Durin::FResolvedStaticMeshView
    {
        using namespace Durin; using namespace RendererPrivate;
        auto Result = Source;
        Result.MeshDrawCommandPasses.clear();
        Result.DrawCommandStorage = std::make_shared<FDynamicMeshDrawCommandStorage>();
        const auto Original = Source.GetMeshDrawCommandPass({EMeshPipelinePass::GBuffer}).GetMeshDrawCommands();
        for (uint32 Index = 0; Index < Prepared.GetNumSections(); ++Index)
        {
            const auto Found = std::ranges::find(Original, Index, &FVisibleMeshDrawCommand::SourceDrawIndex);
            auto Command = *(Found == Original.end() ? Original.front().Command : Found->Command);
            AddTestMeshDrawCommand(*Result.DrawCommandStorage, Result.GetMeshDrawCommandPass({EMeshPipelinePass::GBuffer}), std::move(Command), Index);
        }
        require(SetupMeshDrawCommands(FRHICommandListImmediate::Get(), Prepared, Result, false));
        return Result;
    }
    auto BuildTestInstanceCullingPlan(Durin::FInstanceCullingManager& Manager, const Durin::FPreparedStaticMeshView& Prepared, const Durin::FResolvedStaticMeshView& Resolved,
        const Durin::FSceneView& View, size_t* Overflow = nullptr,
        size_t Budget = Durin::FInstanceCullingContext::MaximumGeneratedBytes,
        Durin::FInstanceCullingAdmissionDiagnostics* Diagnostics = nullptr) -> std::shared_ptr<Durin::FInstanceCullingContext>
    {
        auto Context = Manager.CreateContext(Manager.RegisterView(View), {Durin::RendererPrivate::EMeshPipelinePass::GBuffer});
        const auto Ready = PrepareTestMeshDrawCommands(Prepared, Resolved);
        const auto Span = Ready.GetMeshDrawCommandPass({Durin::RendererPrivate::EMeshPipelinePass::GBuffer}).GetMeshDrawCommands();
        Durin::RendererPrivate::FMeshCommandOneFrameArray Commands(Span.begin(), Span.end());
        return Context->SetupDrawCommands(Commands, Durin::RendererPrivate::BuildMeshDrawCommandCullingPayloads(Prepared, Ready, {Durin::RendererPrivate::EMeshPipelinePass::GBuffer}), Overflow, Budget, Diagnostics) ? Context : nullptr;
    }
}

TEST(FRendererSceneContractTests, GBufferGPUCullingPlanPreservesIndexedArguments)
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
	RendererPrivate::FMeshPassPipelineInitializer PipelineInitializer;
	PipelineInitializer.Pass = RendererPrivate::EMeshPipelinePass::GBuffer;
	class FDeclaration final : public FRHIVertexDeclaration {
        FVertexDeclarationElementList Elements;
        auto GetElements() const -> const FVertexDeclarationElementList& override { return Elements; }
    };
    PipelineInitializer.VertexDeclaration = new FDeclaration;
    auto Pipeline = std::make_shared<const RendererPrivate::FMeshPassPipeline>(std::move(PipelineInitializer));
    const auto IndexBuffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("CullingIndices", 256, 4, EBufferUsageFlags::IndexBuffer));
    GPUCommand->IndexBuffer = IndexBuffer; GPUCommand->DrawParameters = Command->Geometry.GetIndexedDrawArguments();
    GPUCommand->Pipeline = Pipeline.get(); GPUCommand->bFinalized = true;
	Resolved.DrawCommandStorage = std::make_shared<RendererPrivate::FDynamicMeshDrawCommandStorage>();
	RendererPrivate::AddTestMeshDrawCommand(*Resolved.DrawCommandStorage, Resolved.GetMeshDrawCommandPass(Pipeline->GetPassKey()), std::move(*GPUCommand), 0);
	Prepared.Primitives[0].CollectedBinding = std::make_shared<FStaticMeshBatchBinding>();

    auto SceneData = std::make_shared<FGPUSceneBufferSet>();
    SceneData->SceneData[1] = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("CullingSceneInstances",
        64 * sizeof(FInstanceSceneShaderData), 16, EBufferUsageFlags::StructuredBuffer));
    Resolved.GPUScenePrimitives.push_back({SceneData, 0, 0});
	FInstanceCullingManager Manager;
	FSceneView View;
	EXPECT_FALSE(BuildTestInstanceCullingPlan(
		Manager, Prepared, Resolved, View));
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
    {
        auto Ready = PrepareTestMeshDrawCommands(Prepared, Resolved);
        const auto ViewId = Manager.RegisterView(View);
        auto& Pass = Ready.GetMeshDrawCommandPass(Pipeline->GetPassKey());
        EXPECT_FALSE(Pass.GetInstanceCullingContext());
        ASSERT_TRUE(Pass.SetupDrawCommands(Manager, ViewId, RendererPrivate::BuildMeshDrawCommandCullingPayloads(Prepared, Ready, Pipeline->GetPassKey()), nullptr,
            FInstanceCullingContext::MaximumGeneratedBytes, nullptr));
        const auto* Context = Pass.GetInstanceCullingContext();
        ASSERT_TRUE(Context); EXPECT_EQ(Context->GetCandidates().size(), 64u);
        ASSERT_EQ(Pass.GetMeshDrawCommands().size(), 1u);
        for (uint32 Source = 0; Source < 64; ++Source)
            EXPECT_EQ(Pass.GetVisibleForSource(Source), &Pass.GetMeshDrawCommands().front());
        EXPECT_EQ(Pass.GetBasePassRange(EMeshBasePass::Opaque).NumCommands, 1u);
        auto& Other = Ready.GetMeshDrawCommandPass({RendererPrivate::EMeshPipelinePass::Forward});
        EXPECT_FALSE(Other.SetupDrawCommands(Manager, ViewId, {}, nullptr,
            FInstanceCullingContext::MaximumGeneratedBytes, nullptr));
        EXPECT_NE(Other.GetInstanceCullingContext(), Context);
        EXPECT_TRUE(Other.GetInstanceCullingContext()->GetCandidates().empty());
        EXPECT_EQ(Pass.GetInstanceCullingContext(), Context);
        EXPECT_EQ(Context->GetArgumentIndex(0), FInstanceCullingContext::InvalidArgument);
    }

	const auto Plan = BuildTestInstanceCullingPlan(
		Manager, Prepared, Resolved, View);
	ASSERT_TRUE(Plan);
	ASSERT_EQ(Plan->GetCandidates().size(),
		FInstanceCullingContext::MinimumGroupCandidates);
	ASSERT_EQ(Plan->GetArguments().size(), 1u);
	EXPECT_EQ(Plan->GetArguments()[0].IndexCount, 36u);
	EXPECT_EQ(Plan->GetArguments()[0].InstanceCount, 0u);
	EXPECT_EQ(Plan->GetArguments()[0].FirstIndex, 7u);
	EXPECT_EQ(Plan->GetArguments()[0].VertexOffset, -2);
	EXPECT_EQ(Plan->GetArgumentIndex(0), FInstanceCullingContext::InvalidArgument);
    EXPECT_EQ(Plan->GetCandidates()[0].InstanceId, 0u);
    EXPECT_EQ(Plan->GetCandidates()[0].ArgumentIndex, 0u);
    EXPECT_EQ(Plan->GetCandidates().back().InstanceId, 63u);
    // Equivalent independent wrappers must group by effective state. Preserve
    // prepared order: an incompatible middle draw splits the run.
    {
        auto Ready = PrepareTestMeshDrawCommands(Prepared, Resolved);
        const auto Span = Ready.GetMeshDrawCommandPass(Pipeline->GetPassKey()).GetMeshDrawCommands();
        RendererPrivate::FMeshCommandOneFrameArray Visible(Span.begin(), Span.end());
        auto Sources = RendererPrivate::BuildMeshDrawCommandCullingPayloads(Prepared, Ready, Pipeline->GetPassKey());
        std::vector<RendererPrivate::FMeshDrawCommand> Commands(Visible.size(), *Visible.front().Command);
        std::vector<std::shared_ptr<const RendererPrivate::FMeshPassPipeline>> Pipelines; Pipelines.reserve(Visible.size());
        std::vector<RendererPrivate::FMeshPassBindings> Bindings(Visible.size(), *Visible.front().Pass);
        for (size_t Index = 0; Index < Visible.size(); ++Index) {
            auto Initializer = Pipeline->GetInitializer(); Initializer.VertexDeclaration = new FDeclaration;
            Pipelines.push_back(std::make_shared<const RendererPrivate::FMeshPassPipeline>(std::move(Initializer)));
            Commands[Index].Pipeline = Pipelines.back().get(); Visible[Index].Command = &Commands[Index];
            Visible[Index].Pass = &Bindings[Index];
            auto& Data = Sources[Visible[Index].SourceDrawIndex].SceneData;
            Data.Buffers = std::make_shared<FGPUSceneBufferSet>(*Data.Buffers);
        }
        auto Context = Manager.CreateContext(Manager.RegisterView(View), Pipeline->GetPassKey());
        const auto Runs = RendererPrivate::BuildMeshDrawCommandRuns(Visible, Sources);
        ASSERT_EQ(Runs.size(), 1u); EXPECT_EQ(Runs.front().FirstCommand, 0u); EXPECT_EQ(Runs.front().NumCommands, 64u);
        const auto OriginalVisible = Visible;
        EXPECT_TRUE(Context->SetupDrawCommands(Visible, Sources, nullptr, FInstanceCullingContext::MaximumGeneratedBytes, nullptr));
        ASSERT_EQ(Visible.size(), 1u); EXPECT_EQ(Visible[0].NumSourceDraws, 64u);
        Visible = OriginalVisible;
        EXPECT_EQ(Context->GetArguments().size(), 1u); EXPECT_EQ(Context->GetCandidates().size(), 64u);
        Visible[32].DepthBias[0] = 1.f;
        EXPECT_FALSE(Context->SetupDrawCommands(Visible, Sources)); EXPECT_TRUE(Context->GetCandidates().empty());
        Visible[32].DepthBias[0] = 0.f; Bindings[32].Fragment.Shader = FragmentShader;
        EXPECT_FALSE(Context->SetupDrawCommands(Visible, Sources));
        Bindings[32].Fragment.Shader = nullptr;
        Visible.push_back(Visible.back()); ++Visible.back().SourceDrawIndex;
        Sources.push_back(Sources.back()); Sources.back().bSupportsInstanceCulling = false;
        EXPECT_FALSE(Context->SetupDrawCommands(Visible, Sources));
        Visible.pop_back(); Sources.pop_back();
        EXPECT_TRUE(Context->SetupDrawCommands(Visible, Sources));
        Durin::FInstanceCullingContextTestAccess::SetDispatched(*Context, true);
        EXPECT_TRUE(Context->IsCullingDispatched());
        EXPECT_EQ(Context->GetArgumentIndex(0), 0u);
        EXPECT_EQ(Context->GetArgumentIndex(1), FInstanceCullingContext::InvalidArgument);
        EXPECT_EQ(Visible.size(), 1u);
        const auto InvalidSubset = Context->SubmitDrawCommands(Visible, {}, 1, 1, FRHICommandListImmediate::Get());
        EXPECT_FALSE(InvalidSubset.bComplete); EXPECT_EQ(InvalidSubset.NativeSubmissions, 0u);
        Durin::FInstanceCullingContextTestAccess::SetDispatched(*Context, false);
        EXPECT_FALSE(Context->IsCullingDispatched());
        EXPECT_EQ(Context->GetArgumentIndex(0), FInstanceCullingContext::InvalidArgument);
        // A second GPU group is command 1 in the final array, not a skipped member of group 0.
        Visible = OriginalVisible;
        for (auto Item : OriginalVisible) { Sources.push_back(Sources[Item.SourceDrawIndex]); Item.SourceDrawIndex += 64; Item.DepthBias[0] = 1.f; Visible.push_back(Item); }
        ASSERT_TRUE(Context->SetupDrawCommands(Visible, Sources)); ASSERT_EQ(Visible.size(), 2u);
        Durin::FInstanceCullingContextTestAccess::SetDispatched(*Context, true);
        const auto Subset = Context->SubmitDrawCommands(Visible, {}, 1, 1, FRHICommandListImmediate::Get());
        EXPECT_FALSE(Subset.bComplete); EXPECT_EQ(Subset.Attempted, 64u); EXPECT_EQ(Subset.Rejected, 64u);
        EXPECT_EQ(Subset.Successful, 0u); EXPECT_EQ(Subset.NativeSubmissions, 0u);
        Durin::FInstanceCullingContextTestAccess::SetDispatched(*Context, false);
        RendererPrivate::FMeshDrawCommandPassStats FallbackStats;
        const auto Fallback = Context->SubmitDrawCommands(Visible, {}, 1, 1, FRHICommandListImmediate::Get(), nullptr, &FallbackStats);
        EXPECT_EQ(Fallback.Attempted, 64u); EXPECT_EQ(Fallback.Rejected, 64u);
        EXPECT_EQ(FallbackStats.DrawData.size(), 64u);
        EXPECT_EQ(FallbackStats.DrawData.front().SourceDrawIndex, 64u);
    }
    constexpr size_t PerCandidateBytes = sizeof(FInstanceCullingCandidate) + sizeof(uint32);
	const size_t ExactBudget = Plan->GetCandidates().size() * PerCandidateBytes + sizeof(FRHIDrawIndexedIndirectArguments);
	size_t Overflow = 0;
	FInstanceCullingAdmissionDiagnostics Admission;
	EXPECT_TRUE(BuildTestInstanceCullingPlan(Manager, Prepared, Resolved, View, &Overflow, ExactBudget, &Admission));
	EXPECT_EQ(Overflow, 0u);
	EXPECT_EQ(Admission.BelowThresholdCandidates, 0u);
	EXPECT_FALSE(BuildTestInstanceCullingPlan(Manager, Prepared, Resolved, View, &Overflow, ExactBudget - 1));
	EXPECT_EQ(Overflow, 64u);
	EXPECT_FALSE(BuildTestInstanceCullingPlan(Manager, Prepared, Resolved, View, &Overflow, 0));
	EXPECT_EQ(Overflow, 64u);
	{
		auto ManyPrepared = Prepared;
		auto ManyResolved = Resolved;
		const size_t MaximumGroups = std::min(FInstanceCullingContext::MaximumGeneratedBytes / ExactBudget, FInstanceCullingContext::MaximumCandidates / 64);
		for (size_t Group = 1; Group <= MaximumGroups; ++Group)
		{
			auto GroupCommand = std::make_shared<FMeshDrawDescription>(*Command);
            auto GroupDraw = *Resolved.GetMeshDrawCommandPass(Pipeline->GetPassKey()).GetMeshDrawCommands().front().Command;
            std::get<FRHIDrawIndexedArguments>(GroupDraw.DrawParameters).FirstIndex += static_cast<uint32>(Group);
			for (uint32 Index = 0; Index < 64; ++Index)
			{
				auto Draw = Prepared.Opaque[Index];
				Draw.Command = GroupCommand;
				Draw.ResolvedIndex = static_cast<uint32>(ManyResolved.Draws.size());
				ManyPrepared.Opaque.push_back(Draw);
				ManyResolved.Draws.push_back(Resolved.Draws[Index]);
                RendererPrivate::AddTestMeshDrawCommand(*Resolved.DrawCommandStorage, ManyResolved.GetMeshDrawCommandPass(Pipeline->GetPassKey()), GroupDraw, Draw.ResolvedIndex);
			}
		}
		const auto ManyPlan = BuildTestInstanceCullingPlan(Manager, ManyPrepared, ManyResolved, View, &Overflow);
		ASSERT_TRUE(ManyPlan);
		ASSERT_EQ(ManyPlan->GetArguments().size(), MaximumGroups);
		EXPECT_EQ(Overflow, 64u);
		for (size_t Group = 0; Group < MaximumGroups; ++Group)
		{
			EXPECT_EQ(ManyPlan->GetArguments()[Group].FirstInstance, Group * 64);
			EXPECT_EQ(ManyPlan->GetCandidates()[Group * 64].ArgumentIndex, Group);
		}
	}
	{
		auto Small = Prepared;
		Small.Opaque.pop_back();
		EXPECT_FALSE(BuildTestInstanceCullingPlan(Manager, Small, Resolved, View, nullptr,
			FInstanceCullingContext::MaximumGeneratedBytes, &Admission));
		EXPECT_EQ(Admission.BelowThresholdCandidates, 63u);
		Small.Opaque.clear();
		EXPECT_FALSE(BuildTestInstanceCullingPlan(Manager, Small, Resolved, View));
	}

    {
        auto Invalid = Resolved;
        Invalid.GPUScenePrimitives[0].InstanceId = 64;
        EXPECT_FALSE(BuildTestInstanceCullingPlan(Manager, Prepared, Invalid, View, nullptr,
            FInstanceCullingContext::MaximumGeneratedBytes, &Admission));
        EXPECT_EQ(Admission.PreparationUnavailableCandidates, 1u);
        EXPECT_EQ(Admission.BelowThresholdCandidates, 63u);
        auto InstanceInput = std::make_shared<FStaticMeshBatchBinding>();
        InstanceInput->DeclarationElements[0].InputRate = FRHIVertexElementIdentity::EInputRate::Instance;
        auto Unsupported = Prepared;
        for (auto& Primitive : Unsupported.Primitives) Primitive.CollectedBinding = InstanceInput;
        EXPECT_FALSE(BuildTestInstanceCullingPlan(Manager, Unsupported, Resolved, View, nullptr,
            FInstanceCullingContext::MaximumGeneratedBytes, &Admission));
        EXPECT_EQ(Admission.IncompatibleCandidates, 64u);
    }

	for (auto& Primitive : Prepared.Primitives)
		Primitive.VertexDomain = EVertexDeformationDomain::Spline;
	EXPECT_FALSE(BuildTestInstanceCullingPlan(
		Manager, Prepared, Resolved, View));
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
	const auto TranslatedPlan = BuildTestInstanceCullingPlan(Manager, Prepared, Resolved, View);
	ASSERT_TRUE(TranslatedPlan);
	ASSERT_EQ(TranslatedPlan->GetCandidates().size(), Plan->GetCandidates().size());
	for (size_t Index = 0; Index < Plan->GetCandidates().size(); ++Index)
	{
		EXPECT_EQ(TranslatedPlan->GetCandidates()[Index].InstanceId, Plan->GetCandidates()[Index].InstanceId);
	}

}

TEST(FRendererSceneContractTests, PreparedSurfaceBindingsClearPreviousPayloadOnFailure)
{
	using namespace Durin;
	using namespace Durin::RendererPrivate;
	FShaderReflectionData Reflection;
	Reflection.ResourceBindings = {{.Name = "Material", .SetIndex = 2, .BindingIndex = 11,
		.Type = ERHIBindingType::UniformBuffer}};
	auto Schema = BuildShaderParameterSchema(*FSurfaceParameterProviders::GetParametersMetadata());
    ASSERT_TRUE(Schema);
    std::erase_if(Schema->Fields, [](const auto& F) { return F.ShaderName != "Material"; });
    Schema = CanonicalizeShaderParameterSchema(std::move(*Schema)); ASSERT_TRUE(Schema);
    const auto Accepted = DescribeMaterialShaderBindings(Reflection, *Schema); ASSERT_TRUE(Accepted);
    const FCompiledSurfaceBindingLayout Layout(*Accepted);
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
	EXPECT_EQ(Bindings.GetResources()[0].SetIndex, 2u);
	EXPECT_EQ(Bindings.GetResources()[0].BindingIndex, 11u);
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
        std::shared_ptr<const Durin::RendererPrivate::FMeshPassPipeline> Pipeline;
        Durin::FMeshDrawDescription Description;
        explicit FStoredCommandSource(Durin::RendererPrivate::FMeshPassKey Pass)
            : Pipeline(std::make_shared<const Durin::RendererPrivate::FMeshPassPipeline>(Durin::RendererPrivate::FMeshPassPipelineInitializer{.Pass = Pass.Pass})) {}
        auto Key(uint64 Batch) const -> Durin::RendererPrivate::FCachedMeshDrawCommandStorage::FKey
        { return Durin::RendererPrivate::FCachedMeshDrawCommandStorage::MakeKey(&Description, Durin::InvalidPrimitiveComponentId, Batch, *Pipeline); }
        auto MakeCommand(uint64, bool Cached) const -> Durin::RendererPrivate::FMeshDrawCommand
        {
            using namespace Durin; using namespace RendererPrivate;
            FMeshDrawCommand Command; Command.bFinalized = true; Command.bCached = Cached;
            Command.Pipeline = Pipeline.get();
            return Command;
        }
    };
}
TEST(FRendererSceneContractTests, DynamicDrawListContextBuildsInStableStorageBeforePublication)
{
    using namespace Durin; using namespace RendererPrivate;
    const FMeshPassKey PassKey{EMeshPipelinePass::Forward};
    auto Pipeline = std::make_shared<const FMeshPassPipeline>(FMeshPassPipelineInitializer{.Pass = PassKey.Pass});
    FPreparedStaticMeshPrimitive Primitive;
    FPreparedStaticMeshDraw Draw; Draw.ResolvedIndex = 7;
    FResolvedStaticMeshView::FMaterialUniform Material;
    FMeshDrawCommandBuildInput Input{FRHICommandListImmediate::Get(), Primitive, Draw, Pipeline, Material};
    FMeshDrawCommand Initializer; Initializer.Pipeline = Pipeline.get();
    Initializer.PassBindings.Vertex.Parameters.emplace_back();
    FParallelMeshDrawCommandPass Pass(PassKey);
    const FMeshDrawCommand* Stored = nullptr;
    {
        FMeshDrawCommandBindingCache Cache;
        FDynamicMeshDrawCommandStorage Frame;
        FDynamicPassMeshDrawListContext Context(Frame, Pass, Cache);
        auto& Command = Context.AddCommand(Initializer);
        EXPECT_NE(&Command, &Initializer);
        EXPECT_TRUE(Pass.GetMeshDrawCommands().empty());
        // Later allocations and an abandoned build cannot move or publish this command.
        for (uint32 Index = 0; Index < 2048; ++Index) Context.AddCommand(Initializer);
        EXPECT_FALSE(Command.bFinalized); Command.DepthBias = {1.f, 2.f, 3.f};
        Context.FinalizeCommand(Input, Command);
        EXPECT_TRUE(Command.bFinalized);
        ASSERT_EQ(Pass.GetMeshDrawCommands().size(), 1u);
        Stored = &Command;
        EXPECT_EQ(Pass.GetMeshDrawCommands().front().Command, Stored);
        EXPECT_EQ(Pass.GetMeshDrawCommands().front().SourceDrawIndex, 7u);
        EXPECT_FALSE(Initializer.bFinalized);
    }
    // The visible command retains the allocation after its build context and storage facade expire.
    ASSERT_EQ(Pass.GetMeshDrawCommands().size(), 1u);
    EXPECT_EQ(Pass.GetMeshDrawCommands().front().Command, Stored);
    EXPECT_EQ(Stored->PassBindings.Vertex.Parameters.size(), 1u);
    EXPECT_EQ(Stored->DepthBias, (std::array<float, 3>{1.f, 2.f, 3.f}));
}
TEST(FRendererSceneContractTests, CachedPassStorageRetainsLiveCommandsUntilSourceInvalidation)
{
    using namespace Durin; using namespace RendererPrivate;
    const FMeshPassKey Pass{EMeshPipelinePass::Forward};
    FStoredCommandSource DrawSource(Pass);
    FCachedMeshDrawCommandStorage Storage;
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    const auto Held = Storage.Publish(DrawSource.Key(1), DrawSource.MakeCommand(1, true), *Source, DrawSource.Pipeline);
    const auto Key = DrawSource.Key(1);
    for (uint64 I = 0; I < 2050; ++I) Storage.Publish(DrawSource.Key(I + 2), DrawSource.MakeCommand(I + 2, true), *Source, DrawSource.Pipeline);
    EXPECT_EQ(Storage.Num(), 2051u); EXPECT_TRUE(Storage.FindCachedCommand(Key, *Source).Command);
    Source->RemoveCachedMeshDrawCommands();
    EXPECT_EQ(Storage.Num(), 0u); EXPECT_FALSE(Storage.FindCachedCommand(Key, *Source).Command);
    EXPECT_TRUE(Held->bFinalized);
}
TEST(FRendererSceneContractTests, VisibleCommandMemoryDoesNotKeepSourceResourcesAlive)
{
    using namespace Durin; using namespace RendererPrivate;
    FCachedMeshDrawCommandStorage Cache;
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    const FMeshPassKey Pass{EMeshPipelinePass::Forward};
    FStoredCommandSource DrawSource(Pass);
    auto Command = Cache.Publish(DrawSource.Key(41), DrawSource.MakeCommand(41, true), *Source, DrawSource.Pipeline);
    auto Bindings = std::make_shared<FMeshPassBindings>();
    FParallelMeshDrawCommandPass Submission(Pass);
    Submission.AddCachedMeshDrawCommand(Command, Cache.FindCachedCommand(DrawSource.Key(41), *Source).Info, Bindings, {1, 2, 3});
    ASSERT_EQ(Submission.GetMeshDrawCommands().size(), 1u);
    const auto* Borrowed = &Submission.GetMeshDrawCommands().front();
    std::weak_ptr<FPrimitiveSceneInfo> Life = Source;
    Cache.Reset(); Command.reset(); Source.reset();
    EXPECT_TRUE(Life.expired());
    EXPECT_EQ(Borrowed->CachedInfo->CommandIndex, INDEX_NONE);
    EXPECT_TRUE(Borrowed->Command->bFinalized);
	FInstanceCullingManager Manager;
	auto Culling = Manager.CreateContext(Manager.RegisterView(FSceneView{}), Pass);
	FInstanceCullingAdmissionDiagnostics Diagnostics;
	FMeshCommandOneFrameArray Visible(Submission.GetMeshDrawCommands().begin(), Submission.GetMeshDrawCommands().end());
    EXPECT_FALSE(Culling->SetupDrawCommands(Visible, {},
		nullptr, FInstanceCullingContext::MaximumGeneratedBytes, &Diagnostics));
	EXPECT_EQ(Diagnostics.PreparationUnavailableCandidates, 1u);
}
TEST(FRendererSceneContractTests, CachedCommandCannotReviveAReleasedSource)
{
    using namespace Durin; using namespace RendererPrivate;
    FCachedMeshDrawCommandStorage Cache;
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    const FMeshPassKey Pass{EMeshPipelinePass::Forward};
    FStoredCommandSource DrawSource(Pass);
    const auto Command = Cache.Publish(DrawSource.Key(13), DrawSource.MakeCommand(13, true), *Source, DrawSource.Pipeline);
    const auto Key = DrawSource.Key(13);
    ASSERT_TRUE(Cache.FindCachedCommand(Key, *Source).Command);
    Source->RemoveCachedMeshDrawCommands();
    EXPECT_EQ(Cache.Num(), 0u); EXPECT_FALSE(Cache.FindCachedCommand(Key, *Source).Command);
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
    const auto Descriptor = Cache.Publish(Key, DrawSource.MakeCommand(14, true), *Source, DrawSource.Pipeline);
    ASSERT_TRUE(Cache.FindCachedCommand(Key, *Source).Command);
    Slots.FindOrAddBounded(2, 1);
    EXPECT_EQ(Cache.Num(), 1u); EXPECT_TRUE(Cache.FindCachedCommand(Key, *Source).Command);
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
        const auto Cached = Cache.Publish(Key, std::move(Draw), *Source, DrawSource.Pipeline);
        ASSERT_TRUE(Cache.FindCachedCommand(Key, *Source).Command);
        EXPECT_EQ(ObservedBuffer->GetRefCount(), InitialReferences);
        Source->RetainPrimitiveUniformBuffers({});
        EXPECT_EQ(ObservedBuffer->GetRefCount(), InitialReferences - 1);
        EXPECT_EQ(Cache.Num(), 0u); EXPECT_FALSE(Cache.FindCachedCommand(Key, *Source).Command);
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
    Bindings.Parameters = {{A.GetReference(), 2, 7, 0, ERHIBindingType::StorageBuffer, 0, 64}};
    Bindings.InstanceIdsAllocation = std::pair{2u, 7u};
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
        Cache.Publish(DrawSource.Key(1), std::move(Command), *Source, DrawSource.Pipeline);
        Cache.Publish(DrawSource.Key(2), DrawSource.MakeCommand(2, true), *Other, DrawSource.Pipeline);
        FParallelMeshDrawCommandPass Frame(Pass);
        const auto Selected = Cache.FindCachedCommand(DrawSource.Key(1), *Source);
        Frame.AddCachedMeshDrawCommand(Selected.Command, Selected.Info, std::make_shared<FMeshPassBindings>(), {});
        const auto* Original = Uniform.GetRange().Buffer;
        ASSERT_TRUE(Uniform.Update(Commands, FByteBuffer(16, std::byte{1})));
        EXPECT_EQ(Uniform.GetRange().Buffer, Original);
        EXPECT_EQ(Cache.Num(), 2u); EXPECT_NE(Selected.Info->CommandIndex, INDEX_NONE);
        Source->RemoveCachedMeshDrawCommands();
        EXPECT_FALSE(Cache.FindCachedCommand(DrawSource.Key(1), *Source).Command);
        EXPECT_TRUE(Cache.FindCachedCommand(DrawSource.Key(2), *Other).Command); EXPECT_EQ(Selected.Info->CommandIndex, INDEX_NONE);
        Cache.Publish(DrawSource.Key(1), DrawSource.MakeCommand(1, true), *Source, DrawSource.Pipeline);
        Source->RemoveCachedMeshDrawCommands();
        ASSERT_TRUE(Uniform.Update(Commands, FByteBuffer(32)));
        EXPECT_NE(Uniform.GetRange().Buffer, Original);
        EXPECT_EQ(Cache.Num(), 1u); EXPECT_TRUE(Cache.FindCachedCommand(DrawSource.Key(2), *Other).Command);
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
    Cache.Publish(Original, DrawSource.MakeCommand(1, true), *Source, DrawSource.Pipeline);
    auto Replacement = Original;
    ++Replacement.Bindings[1];
    Cache.Publish(Replacement, DrawSource.MakeCommand(1, true), *Source, DrawSource.Pipeline);
    EXPECT_EQ(Cache.Num(), 1u);
    EXPECT_FALSE(Cache.FindCachedCommand(Original, *Source).Command); EXPECT_TRUE(Cache.FindCachedCommand(Replacement, *Source).Command);
}

TEST(FRendererSceneContractTests, SceneDrawSlotsAreIsolatedAndRetiredSlotsCannotReviveVisibleCommands)
{
    using namespace Durin; using namespace RendererPrivate;
    FStoredCommandSource DrawSource({EMeshPipelinePass::Forward});
    auto A = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    auto B = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    FCachedMeshDrawCommandStorage SceneA, SceneB;
    const auto Key = DrawSource.Key(1);
    auto First = SceneA.Publish(Key, DrawSource.MakeCommand(1, true), *A, DrawSource.Pipeline);
    auto Second = SceneB.Publish(Key, DrawSource.MakeCommand(1, true), *B, DrawSource.Pipeline);
    FParallelMeshDrawCommandPass Frame({EMeshPipelinePass::Forward});
    Frame.AddCachedMeshDrawCommand(First, SceneA.FindCachedCommand(Key, *A).Info, std::make_shared<FMeshPassBindings>(), {});
    Frame.AddCachedMeshDrawCommand(Second, SceneB.FindCachedCommand(Key, *B).Info, std::make_shared<FMeshPassBindings>(), {});
    A->RemoveCachedMeshDrawCommands();
    EXPECT_EQ(Frame.GetMeshDrawCommands()[0].CachedInfo->CommandIndex, INDEX_NONE); EXPECT_NE(Frame.GetMeshDrawCommands()[1].CachedInfo->CommandIndex, INDEX_NONE);
    SceneA.Publish(Key, DrawSource.MakeCommand(1, true), *A, DrawSource.Pipeline);
    EXPECT_EQ(Frame.GetMeshDrawCommands()[0].CachedInfo->CommandIndex, INDEX_NONE); EXPECT_TRUE(SceneA.FindCachedCommand(Key, *A).Command);
    EXPECT_TRUE(SceneB.FindCachedCommand(Key, *B).Command);
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
    EnqueueRenderCommand("MaterialSceneInvalidation", [&](FRHICommandListImmediate& Commands) {
        auto& DrawLists = Scene->GetCachedMeshDrawCommands(Commands, {});
        const auto& Primitives = Scene->GetPrimitiveSceneInfos(); ASSERT_EQ(Primitives.size(), 2u);
        FStoredCommandSource DrawSource({EMeshPipelinePass::Forward});
        const auto Key = DrawSource.Key(1);
        const auto First = DrawLists.Publish(Key, DrawSource.MakeCommand(1, true), *Primitives[0], DrawSource.Pipeline);
        const auto Other = DrawLists.Publish(Key, DrawSource.MakeCommand(1, true), *Primitives[1], DrawSource.Pipeline);
        const auto FirstInfo = DrawLists.FindCachedCommand(Key, *Primitives[0]).Info;
        const auto OtherInfo = DrawLists.FindCachedCommand(Key, *Primitives[1]).Info;
        const FMeshDrawDescriptionCache::FKey PreparationKey{1};
        auto Description = std::make_shared<FMeshDrawDescription>();
        DrawLists.MeshPreparation.Store(PreparationKey, Description);
        ASSERT_EQ(DrawLists.MeshPreparation.Find(PreparationKey, Description->Material), Description);
        Durin::Testing::FMaterialTestState Publication;
        Publication.StaticProperties = FMaterialStaticProperties{};
        const auto Revision = Scene->GetHitProxyRevision_RenderThread();
        Durin::Testing::ApplyMaterialTestState(*A, Publication);
        EXPECT_GT(Scene->GetHitProxyRevision_RenderThread(), Revision);
        EXPECT_EQ(FirstInfo->CommandIndex, INDEX_NONE); EXPECT_NE(OtherInfo->CommandIndex, INDEX_NONE);
        EXPECT_EQ(DrawLists.Num(), 1u);
        EXPECT_FALSE(DrawLists.MeshPreparation.Find(PreparationKey, Description->Material));
        Scene->UpdateStaticMeshes();
        const auto Rebuilt = DrawLists.Publish(Key, DrawSource.MakeCommand(1, true), *Primitives[0], DrawSource.Pipeline);
        const auto RebuiltInfo = DrawLists.FindCachedCommand(Key, *Primitives[0]).Info;
        Durin::Testing::FMaterialTestParameter Scalar; Scalar.Id = FGuid::NewGuid(); Scalar.Value = 0.5f;
        Publication.Parameters.push_back(Scalar);
        Durin::Testing::ApplyMaterialTestState(*A, std::move(Publication));
        EXPECT_NE(RebuiltInfo->CommandIndex, INDEX_NONE); EXPECT_NE(OtherInfo->CommandIndex, INDEX_NONE);
        A->ReleaseRHI();
        EXPECT_EQ(RebuiltInfo->CommandIndex, INDEX_NONE); EXPECT_NE(OtherInfo->CommandIndex, INDEX_NONE);
        DrawLists.Reset();
    });
    FlushRenderingCommands();
    Scene.Reset();
    FlushRenderingCommands();
    A = nullptr; B = nullptr;
    FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, PreparationCacheFollowsSceneAndGenerationLifetime)
{
    using namespace Durin;
    FMeshPassRenderingThreadScope RenderingThread;
    FSceneTestOwner SceneA, SceneB;
    const FPrimitiveComponentId Id(19);
    FSceneInterfaceTestAccess::AddPrimitiveProxy(*SceneA, Id,
        std::make_unique<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1));
    const FMeshDrawDescriptionCache::FKey Key{1};
    std::weak_ptr<const FMeshDrawDescription> Lifetime;
    EnqueueRenderCommand("PopulateScenePreparation", [&](FRHICommandListImmediate& Commands) {
        auto& A = SceneA->GetCachedMeshDrawCommands(Commands, {}).MeshPreparation;
        auto& B = SceneB->GetCachedMeshDrawCommands(Commands, {}).MeshPreparation;
        auto Description = std::make_shared<FMeshDrawDescription>();
        Lifetime = Description;
        A.Store(Key, Description);
        EXPECT_FALSE(B.Find(Key, Description->Material));
        B.Store(Key, Description);
        FRenderResourceGeneration Changed;
        ++Changed.Manual;
        EXPECT_EQ(SceneB->GetCachedMeshDrawCommands(Commands, Changed).MeshPreparation.Num(), 0u);
        EXPECT_EQ(A.Find(Key, Description->Material), Description);
    });
    FlushRenderingCommands();
    ASSERT_FALSE(Lifetime.expired());
    FSceneInterfaceTestAccess::RemovePrimitiveProxy(*SceneA, Id);
    FlushRenderingCommands();
    EXPECT_TRUE(Lifetime.expired());

    EnqueueRenderCommand("PopulateSceneForRelease", [&](FRHICommandListImmediate& Commands) {
        auto Description = std::make_shared<FMeshDrawDescription>();
        Lifetime = Description;
        SceneA->GetCachedMeshDrawCommands(Commands, {}).MeshPreparation.Store(Key, std::move(Description));
    });
    FlushRenderingCommands();
    ASSERT_FALSE(Lifetime.expired());
    SceneA.Reset();
    FlushRenderingCommands();
    EXPECT_TRUE(Lifetime.expired());
}

TEST(FRendererSceneContractTests, CachedStateBucketOwnsPipelineUntilSourceInvalidation)
{
    using namespace Durin; using namespace RendererPrivate;
    FStoredCommandSource DrawSource({EMeshPipelinePass::Forward});
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    auto Pipeline = std::make_shared<FMeshPassPipeline>();
    const std::weak_ptr<const FMeshPassPipeline> Lifetime = Pipeline;
    FCachedMeshDrawCommandStorage Scene;
    auto Draw = DrawSource.MakeCommand(1, true); Draw.Pipeline = Pipeline.get();
    const auto Key = FCachedMeshDrawCommandStorage::MakeKey(&DrawSource.Description, InvalidPrimitiveComponentId, 1, *Pipeline);
    const auto Entry = Scene.Publish(Key, std::move(Draw), *Source, Pipeline, nullptr, {});
    const auto Selected = Scene.FindCachedCommand(Key, *Source);
    const auto& Info = Selected.Info;
    ASSERT_TRUE(Selected); EXPECT_EQ(Selected.Command, Entry); EXPECT_EQ(Selected.Info, Info);
    Pipeline.reset();
    EXPECT_FALSE(Lifetime.expired()); EXPECT_NE(Info->CommandIndex, INDEX_NONE);
    Source->RemoveCachedMeshDrawCommands();
    EXPECT_TRUE(Lifetime.expired()); EXPECT_EQ(Info->CommandIndex, INDEX_NONE);
    EXPECT_EQ(Selected.Info->CommandIndex, INDEX_NONE);
    EXPECT_TRUE(Entry->bFinalized);
}

TEST(FRendererSceneContractTests, CombinedCachedQueryKeepsCommandAndInfoMatchingOnRejection)
{
    using namespace Durin; using namespace RendererPrivate;
    FStoredCommandSource DrawSource({EMeshPipelinePass::Forward});
    auto Source = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    auto Other = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(2), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
    FCachedMeshDrawCommandStorage Cache;
    FStaticMeshBatchBinding Input, DifferentInput;
    FResolvedStaticMeshView::FMaterialUniform Material, Output;
    Material.Uniform.Size = 32; Output.Uniform.Size = 77;
    const auto Key = DrawSource.Key(1);
    const auto Command = Cache.Publish(Key, DrawSource.MakeCommand(1, true), *Source, DrawSource.Pipeline, &Input, Material);
    const FCachedMeshDrawCommand Selected = Cache.FindCachedCommand(Key, *Source, &Output, &Input);
    ASSERT_TRUE(Selected); EXPECT_EQ(Selected.Command, Command); EXPECT_EQ(Output.Uniform.Size, 32u);
    Output.Uniform.Size = 77;
    const auto Rejected = Cache.FindCachedCommand(Key, *Source, &Output, &DifferentInput);
    EXPECT_FALSE(Rejected.Command); EXPECT_FALSE(Rejected.Info); EXPECT_EQ(Output.Uniform.Size, 77u);
    auto Changed = Key; ++Changed.Bindings[0];
    EXPECT_FALSE(Cache.FindCachedCommand(Changed, *Source)); EXPECT_FALSE(Cache.FindCachedCommand(Key, *Other));
    Source->RemoveCachedMeshDrawCommands();
    EXPECT_FALSE(Cache.FindCachedCommand(Key, *Source)); EXPECT_EQ(Selected.Info->CommandIndex, INDEX_NONE);
}
