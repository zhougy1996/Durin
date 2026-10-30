#include "Renderers/MeshPassProcessor.h"

#include "DynamicRHI.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialShaderMap.h"
#include "Renderers/MeshPassRelevance.h"
#include "Renderers/MeshRendererShared.h"
#include "RenderResourcePreparation.h"
#include "RenderingThread.h"
#include "Shader/ShaderMapPreparation.h"

namespace Durin::RendererPrivate
{
	struct FMeshPassProcessor::FState
	{
		std::expected<bool, std::string> Result = false;
		std::array<std::vector<FMeshPSOPrecacheVariant>, 2> Variants;
		std::array<size_t, 2> Next{};
		std::array<FShaderMapPreparation, 2> FixedShaders;
		std::array<bool, 2> Requested{};
		bool bVariantsCollected = false, bShadersReady = false;
		FVertexDeclarationRHIRef Declaration;
		FMeshMaterialShaderMap ShaderMap;
		std::shared_ptr<const FMeshVertexShaderBinding> Vertex;
		FRHIShader* Fragment = nullptr;
	};
	FMeshPassProcessor::FMeshPassProcessor(FMeshPSOCollectorContext InContext) : Context(InContext) {}
	FMeshPassProcessor::~FMeshPassProcessor() = default;
	auto FMeshPassProcessor::IsMeshRelevant(const FMaterialPlanningPassIdentity& Material,
		const FPrimitiveRenderParticipation& Participation, bool bSupportsGBuffer) const -> bool
	{ return IsMeshPassRelevant(GetPass(), Material, Participation, bSupportsGBuffer); }
	auto FMeshPassProcessor::SupportsVertexFactory(const FMeshVertexFactoryImplementation& Factory) const -> bool
	{
		// Receivers are collected through Forward before GBuffer routing; shadows
		// have their own admission and must not reject otherwise valid receivers.
		return Factory.GetShaderType(GetVertexFactoryPass())
			&& (GetPass() == EMeshPipelinePass::ShadowDepth || Factory.GetShaderType(MaterialMeshPassForward));
	}
	auto FMeshPassProcessor::GetPSOCollectionResult() const -> std::expected<bool, std::string>
	{ return State ? State->Result : std::expected<bool, std::string>(false); }

	auto FMeshPassProcessor::CollectPSOInitializers(const FSceneRenderPreparationConfig& Config,
		const FMaterialResource& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
		const FPSOPrecacheParams& Params, FPSOPrecacheDataArray& Out) -> void
	{
		if (CollectionPhase) { CollectPhase(Config, Material, VertexFactoryData, Params, Out, *CollectionPhase); return; }
		CollectPhase(Config, Material, VertexFactoryData, Params, Out, true);
		const auto Required = GetPSOCollectionResult();
		if (!Required || !*Required) return;
		CollectPhase(Config, Material, VertexFactoryData, Params, Out, false);
	}
	auto FMeshPassProcessor::CollectPhase(const FSceneRenderPreparationConfig& Config,
		const FMaterialResource& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
		const FPSOPrecacheParams& Params, FPSOPrecacheDataArray& Out, bool bRequired) -> void
	{
		CheckRenderingThread();
		if (!State) State = std::make_unique<FState>();
		if (!State->Result.has_value()) return;
		State->Result = false;
		const auto Fail = [&](std::string Error) { State->Result = std::unexpected(std::move(Error)); };
		if (!Material.CompiledProgram || !Material.CompiledProgram->ShaderMap || !Config.Version
			|| Config.FeatureLevel != Material.CompiledProgram->FeatureLevel)
		{ Fail("Mesh pass collection requires a matching accepted material and scene configuration."); return; }
		const auto Properties = Material.GetStaticProperties();
		const auto& Map = Material.CompiledProgram->GetShaderMap();
		const FMaterialPlanningPassIdentity Identity{
			.ShaderMap = {.RenderLayout = Map.Layout.Identity, .ProgramIdentity = Map.ShaderMapId,
				.BlendMode = Properties.BlendMode, .ShadingModel = Properties.ShadingModel,
				.OpacityMaskThreshold = Properties.OpacityMaskThreshold},
			.bTwoSided = Properties.bTwoSided, .DepthWritePolicy = Properties.DepthWritePolicy};
		const auto Factory = VertexFactoryData.VertexFactoryType
			? FindMeshVertexFactory(VertexFactoryData.VertexFactoryType->GetStableKey()) : nullptr;
		if (!Factory || &Factory->GetType() != VertexFactoryData.VertexFactoryType)
		{ Fail("No registered mesh shader implementation for the precache Vertex Factory."); return; }
		if (!IsEnabled(Config) || !SupportsVertexFactory(*Factory)
			|| !IsMeshRelevant(Identity, Params.Participation, Factory->GetShaderType(MaterialMeshPassGBuffer) != nullptr))
		{ State->Result = true; return; }
		if (!State->bVariantsCollected)
		{
			for (const auto& Variant : GetPSOPrecacheVariants(Config, Params))
				State->Variants[Variant.bRequired ? 0 : 1].push_back(Variant);
			State->bVariantsCollected = true;
		}
		const size_t Group = bRequired ? 0 : 1;
		auto& Next = State->Next[Group];
		const auto& Variants = State->Variants[Group];
		if (Next == Variants.size()) { State->Result = true; return; }
		if (!GDynamicRHI || !IsTaskSchedulerRunning()) { Fail("Mesh pass collection requires RHI and the task scheduler."); return; }
		const auto Fragment = GetFragmentShader(Identity.ShaderMap.BlendMode);
		const uint32 MeshPass = GetVertexFactoryPass();
		if (!Fragment.Type || !Fragment.Resolve) { Fail("Unsupported mesh pass fragment shader."); return; }
		if (!State->Requested[Group])
		{
			auto& Shaders = State->FixedShaders[Group];
			Shaders = FShaderMapPreparation::Request("MeshVertexFactory", {});
			if (!Factory->UsesGeneratedMaterialVertexStage(MeshPass)) Shaders.Add("MeshVertexFactory", {Factory->GetRuntimeRequestName(MeshPass)});
			if (Fragment.FixedModule) Shaders.Add(Fragment.FixedModule, {Fragment.FixedRequest});
			State->Requested[Group] = true;
		}
		const auto FixedReady = State->FixedShaders[Group].Poll();
		if (!FixedReady) { Fail(FixedReady.error()); return; }
		if (!*FixedReady) return;
		FPreparedShaderMapScope Prepared;
		if (!State->bShadersReady)
		{
			const auto Completion = InitializeMaterialShaderMap(*Factory->GetShaderType(MeshPass), *Fragment.Type,
				Factory->GetType(), MeshPass, Identity.ShaderMap, Context.Generation, Material.CompiledProgram.get(), {}, State->ShaderMap);
			if (Completion.Status == EShaderBuildStatus::Pending) return;
			if (!Completion) { Fail(Completion.Error.empty() ? "Mesh pass shader collection was canceled." : std::string(Completion.Error)); return; }
			const auto DeferredBefore = FRenderResourceAdmissionScope::GetDeferredAdmissions();
			State->Vertex = Factory->Resolve(State->ShaderMap, MeshPass);
			State->Fragment = Fragment.Resolve(State->ShaderMap);
			const auto* Vertex = State->Vertex ? State->Vertex->GetRHIShader(false) : nullptr;
			if ((!Vertex || !State->Fragment) && FRenderResourceAdmissionScope::GetDeferredAdmissions() != DeferredBefore) return;
			if (!Vertex || !State->Fragment) { Fail("Mesh pass material shader creation failed."); return; }
			State->bShadersReady = true;
		}
		if (!State->Declaration)
		{
			const auto Elements = VertexFactoryData.GetVertexElements();
			if (!Elements) { Fail("Invalid precache vertex elements."); return; }
			State->Declaration = GDynamicRHI->RHICreateVertexDeclaration(*Elements);
			if (!State->Declaration) { Fail("Precache vertex declaration creation failed."); return; }
		}
		while (Next < Variants.size())
		{
			const auto& Variant = Variants[Next];
			const auto Description = DescribeMeshDraw(MakeMeshPipelineState(Identity, Variant.Raster),
				Params.Topology, Identity.ShaderMap.BlendMode, Variant.Raster.bReversedZ);
			Out.emplace_back(Description.MakeInitializer(State->Vertex->GetRHIShader(false), State->Fragment,
				State->Declaration, State->ShaderMap.GetPipelineLayout()), Variant.bRequired, Context.Priority);
			++Next;
		}
		State->Result = true;
	}
}
