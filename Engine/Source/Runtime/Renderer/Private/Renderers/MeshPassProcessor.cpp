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
	FMeshPassDrawListContext::FMeshPassDrawListContext(const FRenderResourceGeneration& InGeneration,
		const FMaterialCompilerResult* InProgram, const FShaderCompileOptions& InOptions, FMeshPassShaders& InOutShaders)
		: Generation(InGeneration), Program(InProgram), Options(InOptions), OutShaders(&InOutShaders) {}
	FMeshPassDrawListContext::FMeshPassDrawListContext(FName InName, const FMeshPassShaders& InShaders,
		FRHIVertexDeclaration* InDeclaration, FMeshPassPipeline& InOutCommand)
		: Shaders(&InShaders), Declaration(InDeclaration), OutCommand(&InOutCommand), Name(InName) {}
	FMeshPassDrawListContext::FMeshPassDrawListContext(const FMeshPassShaders& InShaders,
		FRHIVertexDeclaration* InDeclaration, FPSOPrecacheDataArray& InOutInitializers, bool InRequired, EPSOPrecachePriority InPriority)
		: Shaders(&InShaders), Declaration(InDeclaration), OutInitializers(&InOutInitializers), bRequired(InRequired), Priority(InPriority) {}

	FMeshPassDrawListContext::FMeshPassDrawListContext(const FMeshDrawCommandBuildInput& Input,
		FMeshDrawCommandBindingCache& Cache, std::shared_ptr<const FMeshDrawCommand>& Out,
		bool bCached, std::shared_ptr<const FMeshDrawCommand> Existing)
		: BindingInput(&Input), BindingCache(&Cache), OutDrawCommand(&Out), ExistingCommand(std::move(Existing)), bCachedOutput(bCached) {}
	FMeshPassDrawListContext::FMeshPassDrawListContext(std::shared_ptr<const FMeshDrawCommand> Prepared,
		FRHIStorageBufferRange InVisibleInstances, FRHIStorageBufferRange InInstanceTransforms,
		std::shared_ptr<const FMeshDrawCommand>& Out)
		: OutDrawCommand(&Out), GPUCommand(std::move(Prepared)), VisibleInstances(InVisibleInstances), InstanceTransforms(InInstanceTransforms) {}

	auto FMeshDrawCommand::SubmitBindings(FRHICommandList& Commands) const -> bool
	{
		if (bCached || !Pipeline || !Pipeline->PipelineState || !VertexBindings
			|| (!Pipeline->bGPUCulling && !VertexPassBindings)
			|| (Pipeline->FragmentLayout && (!FragmentBindings || !FragmentPassBindings))) return false;
		Commands.SetGraphicsPipelineState(*Pipeline->PipelineState);
		Commands.SetPreparedShaderParameters(VertexBindings);
		if (VertexPassBindings && !VertexPassBindings->Bind(Commands)) return false;
		return !FragmentBindings || (FragmentBindings->Bind(Commands) && FragmentPassBindings->Bind(Commands));
	}

	auto FMeshPassProcessor::BuildMeshDrawCommands(const FMeshPassProcessorBatch& Batch, FMeshPassDrawListContext& DrawContext) const -> void
	{
		CheckRenderingThread();
		DrawContext.Result = FShaderCompletion::Canceled();
		DrawContext.FailureCategory = ERenderResourceCreateErrorCategory::InvalidConfiguration;
		const auto& Factory = Batch.Factory;
		const auto MeshPass = GetVertexFactoryPass(Batch.bGPUCulling);
		if (!SupportsVertexFactory(Factory) || !Factory.GetShaderType(MeshPass)
			|| !IsMeshRelevant(Batch.Material, Batch.Participation, Batch.bSupportsGBuffer && Factory.GetShaderType(MaterialMeshPassGBuffer) != nullptr)) return;
		const auto Fragment = GetFragmentShader(Batch.Material.ShaderMap.BlendMode);
		if (!Fragment.Type || !Fragment.Resolve)
		{ DrawContext.Result = FShaderCompletion::Failure("Unsupported mesh pass fragment shader."); return; }
		const auto MatchesBatch = [&](const FMeshDrawDescription& Draw, const FVertexFactoryInputBinding& Input) {
			const auto& Key = Draw.PipelineKey;
			return Draw.Material.PlanningPassIdentity == Batch.Material && Key.Material == Batch.Material
				&& Key.FactoryKey == Factory.GetType().GetStableKey() && Key.LayoutKey == Factory.GetLayoutKey()
				&& Input.GetFactoryKey() == Key.FactoryKey && Input.GetLayoutKey() == Key.LayoutKey
				&& Key.Topology == Batch.Topology && Key.Rasterizer == Batch.PipelineState.Rasterizer
				&& Key.Depth == Batch.PipelineState.Depth && Key.ColorBlend == Batch.PipelineState.ColorBlend;
		};
		if (DrawContext.GPUCommand)
		{
			const auto& Prepared = *DrawContext.GPUCommand;
			if (Prepared.bCached || !Prepared.Description || !Prepared.Input || !Prepared.Pipeline || !Prepared.Pipeline->ShaderMap
				|| !Prepared.Pipeline->VertexShader || !Prepared.Pipeline->bGPUCulling
				|| !Batch.bGPUCulling || !MatchesBatch(*Prepared.Description, *Prepared.Input)
				|| Prepared.Pipeline->VertexFactoryPass != MeshPass || Prepared.Pipeline->FragmentType != Fragment.Type
				|| Prepared.Pipeline->Pass != GetPass() || Prepared.Pipeline->ShaderMap.GetIdentity() != Batch.Material.ShaderMap
				|| Prepared.Pipeline->FactoryKey != Factory.GetType().GetStableKey() || Prepared.Pipeline->LayoutKey != Factory.GetLayoutKey()
				|| !DrawContext.VisibleInstances.Buffer || !DrawContext.InstanceTransforms.Buffer)
			{ DrawContext.Result = FShaderCompletion::Failure("GPU draw context does not match a prepared dynamic command."); return; }
			auto Command = std::make_shared<FMeshDrawCommand>(Prepared);
			const auto& Vertex = *Command->Pipeline->VertexShader;
			Command->VertexBindings = PrepareMeshVertexMaterial(Vertex,
				Vertex.PrepareGPUCulling(DrawContext.VisibleInstances, DrawContext.InstanceTransforms),
				Command->Material.Surface, Command->Material.Uniform, Command->ViewUniform, Command->Material.CollectionUniforms);
			if (!Command->VertexBindings)
			{ DrawContext.Result = FShaderCompletion::Failure("GPU command vertex binding collection failed."); return; }
			*DrawContext.OutDrawCommand = std::move(Command);
			DrawContext.Result = FShaderCompletion{};
			return;
		}
		if (DrawContext.BindingInput)
		{
			const auto& Input = *DrawContext.BindingInput;
			const auto& Draw = Input.Draw;
			const auto& Primitive = Input.Primitive;
			const auto& Pipeline = Input.Pipeline;
			if (!Draw.Command || !Primitive.CollectedBinding || !Pipeline || Input.Commands.IsInsideRenderPass())
			{ DrawContext.Result = FShaderCompletion::Failure("Mesh bindings require an admitted draw and an outside-pass preparation context."); return; }
			const auto& Key = Draw.Command->PipelineKey;
			if (!MatchesBatch(*Draw.Command, *Primitive.CollectedBinding)
				|| Pipeline->Pass != GetPass() || Pipeline->bGPUCulling != Batch.bGPUCulling
				|| !Pipeline->ShaderMap || Pipeline->ShaderMap.GetIdentity() != Batch.Material.ShaderMap
				|| Pipeline->VertexFactoryPass != MeshPass || Pipeline->FragmentType != Fragment.Type
				|| Pipeline->FactoryKey != Key.FactoryKey || Pipeline->LayoutKey != Key.LayoutKey
				|| (DrawContext.bCachedOutput && Input.HitProxy.Buffer))
			{ DrawContext.Result = FShaderCompletion::Failure("Mesh command binding context does not match the selected pass and admitted batch."); return; }
			if (!Pipeline->PipelineState || !Pipeline->VertexShader || !Pipeline->FragmentShader)
			{ DrawContext.Result = FShaderCompletion::Failure("Mesh command binding requires prepared native resources."); return; }
			auto Command = std::make_shared<FMeshDrawCommand>();
			Command->PrimitiveId = Primitive.PrimitiveId; Command->BatchId = Primitive.BatchId;
			Command->Description = Draw.Command; Command->Input = Primitive.CollectedBinding;
			Command->Pipeline = Pipeline; Command->PrimitiveUniform = Input.PrimitiveUniform;
			Command->PrimitiveState = FMeshDrawPrimitiveState::Capture(Primitive);
			Command->Material = Input.Material; Command->bCached = DrawContext.bCachedOutput;
			auto& Cache = *DrawContext.BindingCache;
			const auto& Existing = DrawContext.ExistingCommand;
			if (Existing)
			{
				if (!Existing->bCached || !Existing->Pipeline || Existing->GetKey() != Command->GetKey() || Existing->Input != Command->Input
					|| Existing->PrimitiveState != Command->PrimitiveState
					|| !std::ranges::equal(Existing->Material.CollectionUniforms, Input.Material.CollectionUniforms,
						[](const auto& A, const auto& B) { return A.Buffer == B.Buffer && A.Offset == B.Offset && A.Size == B.Size; }))
				{ DrawContext.Result = FShaderCompletion::Failure("Cached mesh command does not match the dynamic draw dependencies."); return; }
				Command->VertexBindings = Existing->VertexBindings;
				Command->FragmentBindings = Existing->FragmentBindings;
			}
			const auto& Vertex = Pipeline->VertexShader;
			if (!Pipeline->bGPUCulling && !Command->VertexBindings)
			{
				const auto VertexKey = std::make_tuple(Draw.PrimitiveIndex, Draw.MaterialUniformIndex, Input.MaterialSlot, Vertex->GetRHIShader(false));
				if (const auto Found = Cache.Vertices.find(VertexKey); Found != Cache.Vertices.end()) Command->VertexBindings = Found->second;
				else
				{
					Command->VertexBindings = PrepareMeshVertexMaterial(*Vertex, Vertex->Prepare(Input.Commands, Input.PrimitiveUniform, *Primitive.CollectedBinding),
						Input.Material.Surface, Input.Material.Uniform, {}, Input.Material.CollectionUniforms, true);
					if (!Command->VertexBindings) { DrawContext.Result = FShaderCompletion::Failure("Mesh stable vertex binding collection failed."); return; }
					Cache.Vertices.emplace(VertexKey, Command->VertexBindings);
				}
			}
			if (Pipeline->FragmentLayout)
			{
				const auto& Layout = *Pipeline->FragmentLayout;
				const bool UsesPrimitive = std::ranges::any_of(Layout.GetEntries(), [](const auto& Entry) { return Entry.Source == FCompiledSurfaceBindingLayout::ESource::Primitive; });
				const auto FragmentKey = std::make_tuple(Draw.MaterialUniformIndex, Input.MaterialSlot,
					UsesPrimitive ? Draw.PrimitiveIndex : std::numeric_limits<uint32>::max(), Pipeline->FragmentShader.GetReference());
				if (Command->FragmentBindings)
				{
					const auto [Found, Inserted] = Cache.Fragments.try_emplace(FragmentKey, Command->FragmentBindings);
					Command->FragmentBindings = Found->second;
				}
				else if (const auto Found = Cache.Fragments.find(FragmentKey); Found != Cache.Fragments.end())
				{ Command->FragmentBindings = Found->second; ++Cache.SurfaceBindingBatchReuses; }
				else
				{
					auto Bindings = std::make_shared<FPreparedSurfaceMaterialBindings>();
					if (!PrepareCompiledSurfaceMaterial(Pipeline->FragmentShader, Layout.ForSet(1), Input.Material.Surface,
						Input.Material.Uniform, {}, Input.HitProxy, {}, Input.PrimitiveUniform, Input.Material.CollectionUniforms, *Bindings))
					{ DrawContext.Result = FShaderCompletion::Failure("Mesh stable fragment binding collection failed."); return; }
					Command->FragmentBindings = std::move(Bindings); Cache.Fragments.emplace(FragmentKey, Command->FragmentBindings);
					++Cache.PreparedSurfaceBindingBatches;
				}
			}
			if (!DrawContext.bCachedOutput)
			{
				Command->ViewUniform = Input.ViewUniform;
				auto CollectPass = [&](FRHIShader* Shader, const FCompiledSurfaceBindingLayout& Layout,
					std::shared_ptr<const FPreparedSurfaceMaterialBindings>& Out) -> bool {
					const auto PassKey = std::make_pair(Input.MaterialSlot, Shader);
					if (!Input.HitProxy.Buffer)
						if (const auto Found = Cache.Passes.find(PassKey); Found != Cache.Passes.end()) { Out = Found->second; return true; }
					auto Bindings = std::make_shared<FPreparedSurfaceMaterialBindings>();
					if (!PrepareCompiledSurfaceMaterial(Shader, Layout.ForSet(0), Input.Material.Surface, {}, Input.Lighting,
						Input.HitProxy, Input.ViewUniform, {}, {}, *Bindings)) return false;
					Out = std::move(Bindings); if (!Input.HitProxy.Buffer) Cache.Passes.emplace(PassKey, Out);
					return true;
				};
				if (!Pipeline->bGPUCulling)
				{
					if (!Vertex->GetReflection()) { DrawContext.Result = FShaderCompletion::Failure("Mesh vertex reflection is unavailable."); return; }
					auto Reflection = *Vertex->GetReflection();
					// Factory-specific set-1 bindings are collected by the factory;
					// only pass resources belong to the shared surface binding layout.
					std::erase_if(Reflection.ResourceBindings, [](const auto& Binding) { return Binding.SetIndex != 0; });
					if (!CollectPass(Vertex->GetRHIShader(false), FCompiledSurfaceBindingLayout(Reflection), Command->VertexPassBindings))
					{ DrawContext.Result = FShaderCompletion::Failure("Mesh view vertex binding collection failed."); return; }
				}
				if (Pipeline->FragmentLayout && !CollectPass(Pipeline->FragmentShader, *Pipeline->FragmentLayout, Command->FragmentPassBindings))
				{ DrawContext.Result = FShaderCompletion::Failure("Mesh view fragment binding collection failed."); return; }
			}
			*DrawContext.OutDrawCommand = std::move(Command); DrawContext.Result = FShaderCompletion{};
			return;
		}
		if (DrawContext.OutShaders)
		{
			FMeshPassShaders Candidate;
			DrawContext.FailureCategory = ERenderResourceCreateErrorCategory::ShaderCompile;
			DrawContext.Result = InitializeMaterialShaderMap(*Factory.GetShaderType(MeshPass), *Fragment.Type,
				Factory.GetType(), MeshPass, Batch.Material.ShaderMap, DrawContext.Generation,
				DrawContext.Program, DrawContext.Options, Candidate.ShaderMap);
			if (!DrawContext.Result) return;
			const auto DeferredBefore = FRenderResourceAdmissionScope::GetDeferredAdmissions();
			Candidate.VertexShader = Factory.Resolve(Candidate.ShaderMap, MeshPass);
			Candidate.FragmentShader = Fragment.Resolve(Candidate.ShaderMap);
			if (!Candidate.VertexShader || !Candidate.VertexShader->GetRHIShader(false) || !Candidate.FragmentShader)
			{
				DrawContext.FailureCategory = ERenderResourceCreateErrorCategory::RHIResource;
				DrawContext.Result = FRenderResourceAdmissionScope::GetDeferredAdmissions() != DeferredBefore
					? FShaderCompletion::Pending() : FShaderCompletion::Failure("Mesh pass shader creation failed.");
				return;
			}
			Candidate.FragmentLayout = Fragment.ResolveLayout ? Fragment.ResolveLayout(Candidate.ShaderMap) : nullptr;
			Candidate.VertexFactoryPass = MeshPass;
			Candidate.FragmentType = Fragment.Type;
			Candidate.FactoryKey = Factory.GetType().GetStableKey();
			Candidate.LayoutKey = Factory.GetLayoutKey();
			*DrawContext.OutShaders = std::move(Candidate);
			return;
		}
		const auto* Shaders = DrawContext.Shaders;
		if (!Shaders || !DrawContext.Declaration || !Shaders->ShaderMap
			|| Shaders->ShaderMap.GetIdentity() != Batch.Material.ShaderMap
			|| Shaders->FactoryKey != Factory.GetType().GetStableKey() || Shaders->LayoutKey != Factory.GetLayoutKey()
			|| Shaders->VertexFactoryPass != MeshPass
			|| Shaders->FragmentType != Fragment.Type || !Shaders->VertexShader || !Shaders->FragmentShader
			|| !Shaders->VertexShader->GetRHIShader(false))
		{ DrawContext.Result = FShaderCompletion::Failure("Mesh draw context does not match the selected pass shaders."); return; }
		const auto Description = DescribeMeshDraw(Batch.PipelineState, Batch.Topology,
			Batch.Material.ShaderMap.BlendMode, Batch.bReversedZ);
		const auto Initializer = Description.MakeInitializer(Shaders->VertexShader->GetRHIShader(false), Shaders->FragmentShader,
			DrawContext.Declaration, Shaders->ShaderMap.GetPipelineLayout());
		if (DrawContext.OutInitializers)
			DrawContext.OutInitializers->emplace_back(Initializer, DrawContext.bRequired, DrawContext.Priority);
		else
		{
			FMeshPassPipeline Candidate;
			static_cast<FMeshPassShaders&>(Candidate) = *Shaders;
			Candidate.VertexDeclaration = DrawContext.Declaration;
			Candidate.Pass = GetPass(); Candidate.bGPUCulling = Batch.bGPUCulling;
			DrawContext.FailureCategory = ERenderResourceCreateErrorCategory::GraphicsPipeline;
			Candidate.PipelineState = FRenderPipelineRequestScope::Graphics(DrawContext.Name, Initializer);
			if (!Candidate.PipelineState)
			{ DrawContext.Result = FShaderCompletion::Failure("Mesh pass pipeline creation failed."); return; }
			*DrawContext.OutCommand = std::move(Candidate);
		}
		DrawContext.Result = FShaderCompletion{};
	}

	namespace
	{
		constexpr size_t MeshPassSlotCount = static_cast<size_t>(EMeshPipelinePass::HitProxy) + 1;
		struct FPassProcessorEntry
		{
			PassProcessorCreateFunction Create = nullptr;
			std::unique_ptr<FMeshPassProcessor> Prototype;
		};
		struct FPassProcessorRegistry
		{
			std::mutex Mutex;
			std::array<FPassProcessorEntry, MeshPassSlotCount> Entries;
		};
		auto PassProcessorRegistry() -> FPassProcessorRegistry&
		{
			static FPassProcessorRegistry Registry;
			return Registry;
		}
		auto IsValidMeshPass(EMeshPipelinePass Pass) -> bool
		{ return static_cast<size_t>(Pass) < MeshPassSlotCount; }
	}

	FRegisterPassProcessorCreateFunction::FRegisterPassProcessorCreateFunction(
		PassProcessorCreateFunction InCreateFunction, EMeshPipelinePass InPass)
		: CreateFunction(InCreateFunction), Pass(InPass)
	{
		if (!CreateFunction || !IsValidMeshPass(Pass)) return;
		auto& Registry = PassProcessorRegistry();
		std::lock_guard Lock(Registry.Mutex);
		auto& Entry = Registry.Entries[static_cast<size_t>(Pass)];
		if (Entry.Create) return;
		Entry.Create = CreateFunction;
		bRegistered = true;
	}
	FRegisterPassProcessorCreateFunction::~FRegisterPassProcessorCreateFunction()
	{
		if (!bRegistered) return;
		std::unique_ptr<FMeshPassProcessor> Retired;
		{
			auto& Registry = PassProcessorRegistry();
			std::lock_guard Lock(Registry.Mutex);
			auto& Entry = Registry.Entries[static_cast<size_t>(Pass)];
			if (Entry.Create != CreateFunction) return;
			Retired = std::move(Entry.Prototype);
			Entry.Create = nullptr;
		}
	}
	auto FPassProcessorManager::IsRegistered(EMeshPipelinePass Pass) -> bool
	{
		if (!IsValidMeshPass(Pass)) return false;
		auto& Registry = PassProcessorRegistry();
		std::lock_guard Lock(Registry.Mutex);
		return Registry.Entries[static_cast<size_t>(Pass)].Create != nullptr;
	}
	auto FPassProcessorManager::GetRegisteredPasses() -> std::vector<EMeshPipelinePass>
	{
		auto& Registry = PassProcessorRegistry();
		std::lock_guard Lock(Registry.Mutex);
		std::vector<EMeshPipelinePass> Passes;
		for (size_t Index = 0; Index < Registry.Entries.size(); ++Index)
			if (Registry.Entries[Index].Create) Passes.push_back(static_cast<EMeshPipelinePass>(Index));
		return Passes;
	}
	auto FPassProcessorManager::CreateMeshPassProcessor(EMeshPipelinePass Pass, FMeshPSOCollectorContext Context)
		-> std::unique_ptr<FMeshPassProcessor>
	{
		if (!IsValidMeshPass(Pass)) return {};
		PassProcessorCreateFunction Create = nullptr;
		{
			auto& Registry = PassProcessorRegistry();
			std::lock_guard Lock(Registry.Mutex);
			Create = Registry.Entries[static_cast<size_t>(Pass)].Create;
		}
		// Factories can query other passes; never invoke them under the registry lock.
		auto Processor = Create ? Create(Context) : nullptr;
		if (Processor && Processor->GetPass() != Pass) return {};
		return Processor;
	}
	auto FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass Pass) -> const FMeshPassProcessor&
	{
		requiref(IsValidMeshPass(Pass), "Invalid mesh pass processor index.");
		auto& Registry = PassProcessorRegistry();
		PassProcessorCreateFunction Create = nullptr;
		{
			std::lock_guard Lock(Registry.Mutex);
			const auto& Entry = Registry.Entries[static_cast<size_t>(Pass)];
			if (Entry.Prototype) return *Entry.Prototype;
			Create = Entry.Create;
		}
		requiref(Create != nullptr, "Unregistered mesh pass processor.");
		auto Prototype = Create({});
		requiref(Prototype && Prototype->GetPass() == Pass, "Mesh pass factory returned an incompatible processor.");
		std::lock_guard Lock(Registry.Mutex);
		auto& Entry = Registry.Entries[static_cast<size_t>(Pass)];
		requiref(Entry.Create == Create, "Mesh pass registration changed during lookup.");
		if (!Entry.Prototype) Entry.Prototype = std::move(Prototype);
		return *Entry.Prototype;
	}

	auto FMeshPassProcessor::RasterVariants(const FSceneRenderPreparationConfig& Config, const FPSOPrecacheParams& Params,
		uint8 Count, bool bShadow, bool bPicking) -> std::vector<FMeshPSOPrecacheVariant>
	{
		std::vector<FMeshPSOPrecacheVariant> Result;
		for (uint8 Variant = 0; Variant < Count; ++Variant)
		{
			const FMeshPipelineRasterOptions Raster{(Variant & 4) != 0, (Variant & 1) != 0,
				Params.bOrientationChanging, (Variant & 2) != 0};
			if (Raster.bWireframe && !Config.bSupportsWireframe) continue;
			Result.push_back({Raster, bShadow || (!bPicking
				&& Raster.bWireframe == Params.bWireframe && Raster.bReversedZ == Params.bReversedZ)});
		}
		return Result;
	}

	struct FMeshPassProcessor::FState
	{
		std::expected<bool, std::string> Result = false;
		std::array<std::vector<FMeshPSOPrecacheVariant>, 2> Variants;
		std::array<size_t, 2> Next{};
		std::array<FShaderMapPreparation, 2> FixedShaders;
		std::array<bool, 2> Requested{};
		bool bVariantsCollected = false, bShadersReady = false;
		FVertexDeclarationRHIRef Declaration;
		FMeshPassShaders Shaders;
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
			const FMeshPassProcessorBatch Batch{Identity, *Factory, {}, Params.Topology, Params.Participation};
			FMeshPassDrawListContext DrawContext(Context.Generation, Material.CompiledProgram.get(), {}, State->Shaders);
			AddMeshBatch(Batch, DrawContext);
			const auto& Completion = DrawContext.GetResult();
			if (Completion.Status == EShaderBuildStatus::Pending) return;
			if (!Completion) { Fail(Completion.Error.empty() ? "Mesh pass shader collection was canceled." : std::string(Completion.Error)); return; }
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
			const FMeshPassProcessorBatch Batch{Identity, *Factory, MakeMeshPipelineState(Identity, Variant.Raster),
				Params.Topology, Params.Participation, true, false, Variant.Raster.bReversedZ};
			FMeshPassDrawListContext DrawContext(State->Shaders, State->Declaration, Out, Variant.bRequired, Context.Priority);
			AddMeshBatch(Batch, DrawContext);
			if (!DrawContext.GetResult()) { Fail("Mesh pass initializer construction failed."); return; }
			++Next;
		}
		State->Result = true;
	}
}
