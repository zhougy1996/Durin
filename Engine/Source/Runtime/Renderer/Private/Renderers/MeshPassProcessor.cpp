#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MaterialPSOPrecacheCollector.h"

#include "DynamicRHI.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialShaderMap.h"
#include "Renderers/MeshPassRelevance.h"
#include "Renderers/MeshRenderingCommon.h"
#include "Renderers/MeshRendererShared.h"
#include "RenderResourcePreparation.h"
#include "RenderingThread.h"
#include "Shader/ShaderMapPreparation.h"

namespace Durin::RendererPrivate
{
	auto FCachedMeshDrawCommandStorage::Find(FMeshPassKey Pass, const FMeshDrawCommand::FKey& Key) -> std::shared_ptr<const FMeshDrawCommand>
	{
		const auto List = Passes.find(Pass); if (List == Passes.end()) return {};
		const auto Found = List->second.find(Key); if (Found == List->second.end()) return {};
		Found->second.LastUsed = ++Serial;
		return Found->second.Command;
	}
	auto FCachedMeshDrawCommandStorage::Publish(FMeshDrawCommand Command) -> std::shared_ptr<const FMeshDrawCommand>
	{
		require(Command.bFinalized && Command.bCached && Command.Pipeline);
		auto Owner = std::make_shared<const FMeshDrawCommand>(std::move(Command));
		auto& List = Passes[Owner->Pipeline->GetPassKey()];
		const auto [Entry, Added] = List.try_emplace(Owner->GetKey());
		if (Added) ++Count;
		Entry->second = {Owner, ++Serial};
		// Retain the existing 2048-command budget across all pass/variant lists.
		while (Count > 2048)
		{
			uint64 Oldest = std::numeric_limits<uint64>::max(); FMeshPassKey OldPass; FMeshDrawCommand::FKey OldKey{};
			for (const auto& [Pass, Commands] : Passes)
				for (const auto& [Key, Value] : Commands)
					if (Value.LastUsed < Oldest) { Oldest = Value.LastUsed; OldPass = Pass; OldKey = Key; }
			auto& Commands = Passes.at(OldPass); Commands.erase(OldKey); --Count;
			if (Commands.empty()) Passes.erase(OldPass);
		}
		return Owner;
	}
	auto FCachedMeshDrawCommandStorage::Reset() -> void { Passes.clear(); Count = 0; Serial = 0; }
	FDynamicMeshDrawCommandStorage::FDynamicMeshDrawCommandStorage() : Data(std::make_shared<FData>())
	{
		static std::atomic<uint64> NextId{1};
		Data->Id = NextId.fetch_add(1, std::memory_order_relaxed); require(Data->Id != 0);
	}
	auto FDynamicMeshDrawCommandStorage::Publish(FMeshPassKey Pass, FMeshDrawCommand Command, FMeshDrawCommandHandle Replace) -> FMeshDrawCommandHandle
	{
		require(Command.bFinalized && !Command.bCached && Command.Pipeline && Command.Pipeline->GetPassKey() == Pass);
		require(!Replace || (Replace.Pass == Pass && GetCommand(Replace)));
		Data->Commands.push_back(std::move(Command));
		auto& List = Data->Passes[Pass];
		if (Replace) { List[Replace.Index] = &Data->Commands.back(); return Replace; }
		List.push_back(&Data->Commands.back());
		return {Pass, List.size() - 1, Data->Id};
	}
	auto FDynamicMeshDrawCommandStorage::GetCommand(FMeshDrawCommandHandle Handle) const -> std::shared_ptr<const FMeshDrawCommand>
	{
		if (!Handle || Handle.StorageId != Data->Id) return {};
		const auto List = Data->Passes.find(Handle.Pass);
		if (List == Data->Passes.end() || Handle.Index >= List->second.size()) return {};
		return std::shared_ptr<const FMeshDrawCommand>(Data, List->second[Handle.Index]);
	}
	auto FDynamicMeshDrawCommandStorage::Num(FMeshPassKey Pass) const -> size_t
	{ const auto Found = Data->Passes.find(Pass); return Found == Data->Passes.end() ? 0 : Found->second.size(); }
	auto FMeshPassDrawListContext::AddCommand(const FMeshDrawCommand& Initializer) -> FMeshDrawCommand&
	{ Pending.emplace(Initializer); return *Pending; }
	auto FMeshPassDrawListContext::TakeCommand(FMeshPassKey Pass) -> FMeshDrawCommand
	{
		require(Pending && Pending->bFinalized && Pending->bCached == IsCached()
			&& Pending->Pipeline && Pending->Pipeline->GetPassKey() == Pass);
		auto Command = std::move(*Pending); Pending.reset(); return Command;
	}
	auto FCachedPassMeshDrawListContext::FinalizeCommand() -> void
	{ Output = Storage.Publish(TakeCommand(Pass)); }
	auto FDynamicPassMeshDrawListContext::FinalizeCommand() -> void
	{ Output = Storage.Publish(Pass, TakeCommand(Pass), Replace); }

	auto FMeshDrawCommand::SetDrawParametersAndFinalize(const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Draw) -> bool
	{
		if (!Draw.Command || !Primitive.CollectedBinding || !Primitive.CollectedBinding->Declaration
			|| (Pipeline && Draw.Command->Geometry.Topology != Pipeline->Description.Topology)
			|| !Draw.Command->Vertices.IsValid()
			|| (Draw.Command->Geometry.bIndexed && (!Draw.Command->Indices.IsValid()
				|| Draw.Command->Indices.Range.ByteOffset > std::numeric_limits<uint32>::max()))
			|| Draw.Command->Geometry.Validate(Draw.Command->Vertices.Range, Draw.Command->Indices.Range) != EGeometrySubmissionOutcome::Submitted
			|| Primitive.CollectedBinding->ValidateInputs(Draw.Command->Geometry) != EGeometrySubmissionOutcome::Submitted) return false;
		VertexStreams = Primitive.CollectedBinding->Streams;
		IndexBuffer = Draw.Command->Geometry.bIndexed ? Draw.Command->Indices.Buffer : FBufferRHIRef{};
		IndexBufferOffset = IndexBuffer ? static_cast<uint32>(Draw.Command->Indices.Range.ByteOffset) : 0;
		if (IndexBuffer) DrawParameters = Draw.Command->Geometry.GetIndexedDrawArguments();
		else DrawParameters = Draw.Command->Geometry.GetDrawArguments();
		IndirectArgs = {}; IndirectArgsOffset = 0;
		bFinalized = true;
		return true;
	}

	auto FMeshDrawCommand::SubmitDraw(FRHICommandList& Commands, FMeshDrawCommandStateCache* StateCache) const -> bool
	{
		if (!bFinalized || bCached || !Commands.IsInsideRenderPass() || !Pipeline
			|| (Pipeline->bGPUCulling && !IndirectArgs)
			|| !std::ranges::all_of(DepthBias, [](float Value) { return std::isfinite(Value); })) return false;
		FMeshDrawCommandStateCache Local;
		auto& Cache = StateCache ? *StateCache : Local;
		if (Cache.VertexPass != VertexPassBindings || Cache.FragmentPass != FragmentPassBindings) Cache = {};
		if (!Cache.Apply(Pipeline.get(), VertexBindings.get(), FragmentBindings.get(), [&] { return SubmitBindings(Commands); })) return false;
		Cache.PipelineOwner = Pipeline; Cache.VertexOwner = VertexBindings; Cache.FragmentOwner = FragmentBindings;
		Cache.VertexPass = VertexPassBindings; Cache.FragmentPass = FragmentPassBindings;
		for (const auto& Stream : VertexStreams) Commands.BindVertexBuffer(Stream.StreamIndex, Stream.VertexBuffer, Stream.Offset);
		if (IndexBuffer) Commands.BindIndexBuffer(IndexBuffer, IndexBufferOffset);
		Commands.SetDepthBias(DepthBias[0], DepthBias[2], DepthBias[1]);
		if (IndirectArgs)
		{
			if (IndexBuffer) Commands.DrawIndexedIndirect(IndirectArgs, IndirectArgsOffset);
			else Commands.DrawIndirect(IndirectArgs, IndirectArgsOffset);
		}
		else if (const auto* Indexed = std::get_if<FRHIDrawIndexedArguments>(&DrawParameters)) Commands.DrawIndexed(*Indexed);
		else Commands.Draw(std::get<FRHIDrawArguments>(DrawParameters));
		return true;
	}

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

	auto FMeshPassProcessor::SelectMeshPass(const FMeshPassProcessorBatch& Batch, FSelection& Out) const -> FShaderCompletion
	{
		CheckRenderingThread();
		const auto& Factory = Batch.Factory;
		const auto MeshPass = GetVertexFactoryPass(Batch.bGPUCulling);
		if (!SupportsVertexFactory(Factory) || !Factory.GetShaderType(MeshPass)
			|| !IsMeshRelevant(Batch.Material, Batch.Participation, Batch.bSupportsGBuffer && Factory.GetShaderType(MaterialMeshPassGBuffer) != nullptr))
			return FShaderCompletion::Canceled();
		const auto Fragment = GetFragmentShader(Batch.Material.ShaderMap.BlendMode);
		if (!Fragment.Type || !Fragment.Resolve) return FShaderCompletion::Failure("Unsupported mesh pass fragment shader.");
		Out = {MeshPass, Fragment, DescribeMeshDraw(Batch.PipelineState, Batch.Topology,
			Batch.Material.ShaderMap.BlendMode, Batch.bReversedZ)};
		return FShaderCompletion{};
	}
	auto FMeshPassProcessor::MatchesBatch(const FMeshPassProcessorBatch& Batch, const FMeshDrawDescription& Draw,
		const FVertexFactoryInputBinding& Input) const -> bool
	{
		const auto& Key = Draw.PipelineKey;
		return Draw.Material.PlanningPassIdentity == Batch.Material && Key.Material == Batch.Material
			&& Key.FactoryKey == Batch.Factory.GetType().GetStableKey() && Key.LayoutKey == Batch.Factory.GetLayoutKey()
			&& Input.GetFactoryKey() == Key.FactoryKey && Input.GetLayoutKey() == Key.LayoutKey
			&& Key.Topology == Batch.Topology && Key.Rasterizer == Batch.PipelineState.Rasterizer
			&& Key.Depth == Batch.PipelineState.Depth && Key.ColorBlend == Batch.PipelineState.ColorBlend;
	}
	auto FMeshPassProcessor::BuildGPUCullingCommand(const FMeshPassProcessorBatch& Batch, const FMeshDrawCommand& Prepared,
		FRHIStorageBufferRange VisibleInstances, FRHIStorageBufferRange InstanceTransforms,
		FRHIBuffer* IndirectArgs, uint64 IndirectArgsOffset, FMeshPassDrawListContext& DrawContext) const -> FShaderCompletion
	{
		FSelection Selection;
		const auto Selected = SelectMeshPass(Batch, Selection);
		if (!Selected) return Selected;
		const auto& Factory = Batch.Factory;
		const auto MeshPass = Selection.MeshPass;
		const auto& Fragment = Selection.Fragment;
		const auto& Description = Selection.Description;
		if (DrawContext.IsCached()) return FShaderCompletion::Failure("GPU graph buffers require a dynamic command list.");
		if (Prepared.bCached || !Prepared.Description || !Prepared.Input || !Prepared.Pipeline || !Prepared.Pipeline->ShaderMap
			|| !Prepared.Pipeline->VertexShader || !Prepared.Pipeline->bGPUCulling
			|| !Batch.bGPUCulling || !MatchesBatch(Batch, *Prepared.Description, *Prepared.Input)
			|| Prepared.Pipeline->VertexFactoryPass != MeshPass || Prepared.Pipeline->FragmentType != Fragment.Type
			|| Prepared.Pipeline->Pass != GetPass() || Prepared.Pipeline->ShaderMap.GetIdentity() != Batch.Material.ShaderMap
			|| Prepared.Pipeline->Description != Description
			|| Prepared.Pipeline->FactoryKey != Factory.GetType().GetStableKey() || Prepared.Pipeline->LayoutKey != Factory.GetLayoutKey()
			|| !VisibleInstances.Buffer || !InstanceTransforms.Buffer)
		{ return FShaderCompletion::Failure("GPU draw context does not match a prepared dynamic command."); }
		auto* Command = &DrawContext.AddCommand(Prepared);
		const auto* Args = IndirectArgs;
		if (!Command->bFinalized || !Command->IndexBuffer || !Args
			|| !EnumHasAnyFlags(Args->GetUsage(), EBufferUsageFlags::DrawIndirect)
			|| IndirectArgsOffset % sizeof(uint32) != 0 || IndirectArgsOffset > Args->GetSize()
			|| sizeof(FRHIDrawIndexedIndirectArguments) > Args->GetSize() - IndirectArgsOffset)
		{ return FShaderCompletion::Failure("GPU command requires a valid indexed indirect argument range."); }
		Command->IndirectArgs = IndirectArgs; Command->IndirectArgsOffset = IndirectArgsOffset;
		const auto& Vertex = *Command->Pipeline->VertexShader;
		Command->VertexBindings = PrepareMeshVertexMaterial(Vertex,
			Vertex.PrepareGPUCulling(VisibleInstances, InstanceTransforms),
			Command->Material.Surface, Command->Material.Uniform, Command->ViewUniform, Command->Material.CollectionUniforms);
		if (!Command->VertexBindings)
		{ return FShaderCompletion::Failure("GPU command vertex binding collection failed."); }
		DrawContext.FinalizeCommand();
		return FShaderCompletion{};
	}
	auto FMeshPassProcessor::BuildMeshDrawCommands(const FMeshPassProcessorBatch& Batch, const FMeshDrawCommandBuildInput& Input,
		FMeshDrawCommandBindingCache& Cache, FMeshPassDrawListContext& DrawContext,
		std::shared_ptr<const FMeshDrawCommand> Existing) const -> FShaderCompletion
	{
		FSelection Selection;
		const auto Selected = SelectMeshPass(Batch, Selection);
		if (!Selected) return Selected;
		const auto MeshPass = Selection.MeshPass;
		const auto& Fragment = Selection.Fragment;
		const auto& Description = Selection.Description;
		const auto& Draw = Input.Draw;
		const auto& Primitive = Input.Primitive;
		const auto& Pipeline = Input.Pipeline;
		// Compatible base-pass routes share material/pass packets; variant shaders
		// still distinguish vertex bindings. HitProxy keeps its request-local domain.
		const auto BindingPass = GetPass() == EMeshPipelinePass::HybridRetainedForward || GetPass() == EMeshPipelinePass::SortedTranslucency
			? EMeshPipelinePass::Forward : GetPass();
		if (!Draw.Command || !Primitive.CollectedBinding || !Pipeline || Input.Commands.IsInsideRenderPass())
		{ return FShaderCompletion::Failure("Mesh bindings require an admitted draw and an outside-pass preparation context."); }
		const auto& Key = Draw.Command->PipelineKey;
		if (!MatchesBatch(Batch, *Draw.Command, *Primitive.CollectedBinding)
			|| Pipeline->Pass != GetPass() || Pipeline->bGPUCulling != Batch.bGPUCulling
			|| Pipeline->Description != Description
			|| !Pipeline->ShaderMap || Pipeline->ShaderMap.GetIdentity() != Batch.Material.ShaderMap
			|| Pipeline->VertexFactoryPass != MeshPass || Pipeline->FragmentType != Fragment.Type
			|| Pipeline->FactoryKey != Key.FactoryKey || Pipeline->LayoutKey != Key.LayoutKey
			|| (DrawContext.IsCached() && Input.HitProxy.Buffer))
		{ return FShaderCompletion::Failure("Mesh command binding context does not match the selected pass and admitted batch."); }
		if (!Pipeline->PipelineState || !Pipeline->VertexShader || !Pipeline->FragmentShader)
		{ return FShaderCompletion::Failure("Mesh command binding requires prepared native resources."); }
		auto* Command = &DrawContext.AddCommand({});
		Command->PrimitiveId = Primitive.PrimitiveId; Command->BatchId = Primitive.BatchId;
		Command->Description = Draw.Command; Command->Input = Primitive.CollectedBinding;
		Command->Pipeline = Pipeline; Command->PrimitiveUniform = Input.PrimitiveUniform;
		Command->PrimitiveState = FMeshDrawPrimitiveState::Capture(Primitive);
		Command->Material = Input.Material; Command->bCached = DrawContext.IsCached();
		if (!Command->SetDrawParametersAndFinalize(Primitive, Draw))
		{ return FShaderCompletion::Failure("Mesh command geometry finalization failed."); }
		const auto Raster = GetPass() == EMeshPipelinePass::ShadowDepth
			? MakeShadowRasterizerState(Batch.PipelineState.Rasterizer) : Pipeline->Description.Rasterizer;
		Command->DepthBias = {Raster.DepthBiasConstantFactor, Raster.DepthBiasSlopeFactor, Raster.DepthBiasClamp};
		if (!Command->bCached && GetPass() == EMeshPipelinePass::ShadowDepth && Draw.RasterBias) Command->DepthBias = *Draw.RasterBias;
		if (Existing)
		{
			const auto SameRange = [](const auto& A, const auto& B) {
				return A.Buffer == B.Buffer && A.Offset == B.Offset && A.Size == B.Size;
			};
			if (!Existing->bCached || !Existing->Pipeline || Existing->GetKey() != Command->GetKey() || Existing->Input != Command->Input
				|| Existing->PrimitiveState != Command->PrimitiveState
				|| Input.HitProxy.Buffer || !SameRange(Existing->PrimitiveUniform, Input.PrimitiveUniform)
				|| !SameRange(Existing->Material.Uniform, Input.Material.Uniform)
				|| Existing->Material.Surface.CompiledTextures != Input.Material.Surface.CompiledTextures
				|| Existing->Material.Surface.CompiledSamplers != Input.Material.Surface.CompiledSamplers
				|| !std::ranges::equal(Existing->Material.CollectionUniforms, Input.Material.CollectionUniforms,
					SameRange))
			{ return FShaderCompletion::Failure("Cached mesh command does not match the dynamic draw dependencies."); }
			Command->VertexBindings = Existing->VertexBindings;
			Command->FragmentBindings = Existing->FragmentBindings;
		}
		const auto& Vertex = Pipeline->VertexShader;
		if (!Pipeline->bGPUCulling && !Command->VertexBindings)
		{
			const auto VertexKey = std::make_tuple(Draw.PrimitiveIndex, Draw.MaterialUniformIndex, BindingPass, Vertex->GetRHIShader(false));
			if (const auto Found = Cache.Vertices.find(VertexKey); Found != Cache.Vertices.end()) Command->VertexBindings = Found->second;
			else
			{
				Command->VertexBindings = PrepareMeshVertexMaterial(*Vertex, Vertex->Prepare(Input.Commands, Input.PrimitiveUniform, *Primitive.CollectedBinding),
					Input.Material.Surface, Input.Material.Uniform, {}, Input.Material.CollectionUniforms, true);
				if (!Command->VertexBindings) { return FShaderCompletion::Failure("Mesh stable vertex binding collection failed."); }
				Cache.Vertices.emplace(VertexKey, Command->VertexBindings);
			}
		}
		if (Pipeline->FragmentLayout)
		{
			const auto& Layout = *Pipeline->FragmentLayout;
			const bool UsesPrimitive = std::ranges::any_of(Layout.GetEntries(), [](const auto& Entry) { return Entry.Source == FCompiledSurfaceBindingLayout::ESource::Primitive; });
			const auto FragmentKey = std::make_tuple(Draw.MaterialUniformIndex, BindingPass,
				UsesPrimitive ? Draw.PrimitiveIndex : std::numeric_limits<uint32>::max(), Pipeline->FragmentShader.GetReference());
			if (Command->FragmentBindings)
			{
				const auto [Found, Inserted] = Cache.Fragments.try_emplace(FragmentKey, Command->FragmentBindings);
				Command->FragmentBindings = Found->second;
			}
			else if (const auto Found = Cache.Fragments.find(FragmentKey); !Input.HitProxy.Buffer && Found != Cache.Fragments.end())
			{ Command->FragmentBindings = Found->second; ++Cache.SurfaceBindingBatchReuses; }
			else
			{
				auto Bindings = std::make_shared<FPreparedSurfaceMaterialBindings>();
				if (!PrepareCompiledSurfaceMaterial(Pipeline->FragmentShader, Layout.ForSet(1), Input.Material.Surface,
					Input.Material.Uniform, {}, Input.HitProxy, {}, Input.PrimitiveUniform, Input.Material.CollectionUniforms, *Bindings))
				{ return FShaderCompletion::Failure("Mesh stable fragment binding collection failed."); }
				Command->FragmentBindings = std::move(Bindings);
				if (!Input.HitProxy.Buffer) Cache.Fragments.emplace(FragmentKey, Command->FragmentBindings);
				++Cache.PreparedSurfaceBindingBatches;
			}
		}
		if (!DrawContext.IsCached())
		{
			Command->ViewUniform = Input.ViewUniform;
			auto CollectPass = [&](FRHIShader* Shader, const FCompiledSurfaceBindingLayout& Layout,
				std::shared_ptr<const FPreparedSurfaceMaterialBindings>& Out) -> bool {
				const auto PassKey = std::make_pair(BindingPass, Shader);
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
				if (!Vertex->GetReflection()) { return FShaderCompletion::Failure("Mesh vertex reflection is unavailable."); }
				auto Reflection = *Vertex->GetReflection();
				// Factory-specific set-1 bindings are collected by the factory;
				// only pass resources belong to the shared surface binding layout.
				std::erase_if(Reflection.ResourceBindings, [](const auto& Binding) { return Binding.SetIndex != 0; });
				if (!CollectPass(Vertex->GetRHIShader(false), FCompiledSurfaceBindingLayout(Reflection), Command->VertexPassBindings))
				{ return FShaderCompletion::Failure("Mesh view vertex binding collection failed."); }
			}
			if (Pipeline->FragmentLayout && !CollectPass(Pipeline->FragmentShader, *Pipeline->FragmentLayout, Command->FragmentPassBindings))
			{ return FShaderCompletion::Failure("Mesh view fragment binding collection failed."); }
		}
		DrawContext.FinalizeCommand(); return FShaderCompletion{};
	}
	auto FMeshPassProcessor::PrepareShaders(const FMeshPassProcessorBatch& Batch, const FRenderResourceGeneration& Generation,
		const FMaterialCompilerResult* Program, const FShaderCompileOptions& Options, FMeshPassShaders& Out,
		ERenderResourceCreateErrorCategory* FailureCategory) const -> FShaderCompletion
	{
		if (FailureCategory) *FailureCategory = ERenderResourceCreateErrorCategory::InvalidConfiguration;
		FSelection Selection;
		const auto Selected = SelectMeshPass(Batch, Selection);
		if (!Selected) return Selected;
		const auto& Factory = Batch.Factory;
		const auto MeshPass = Selection.MeshPass;
		const auto& Fragment = Selection.Fragment;
		FMeshPassShaders Candidate;
		if (FailureCategory) *FailureCategory = ERenderResourceCreateErrorCategory::ShaderCompile;
		auto Result = InitializeMaterialShaderMap(*Factory.GetShaderType(MeshPass), *Fragment.Type,
			Factory.GetType(), MeshPass, Batch.Material.ShaderMap, Generation,
			Program, Options, Candidate.ShaderMap);
		if (!Result) return Result;
		const auto DeferredBefore = FRenderResourceAdmissionScope::GetDeferredAdmissions();
		Candidate.VertexShader = Factory.Resolve(Candidate.ShaderMap, MeshPass);
		Candidate.FragmentShader = Fragment.Resolve(Candidate.ShaderMap);
		if (!Candidate.VertexShader || !Candidate.VertexShader->GetRHIShader(false) || !Candidate.FragmentShader)
		{
			if (FailureCategory) *FailureCategory = ERenderResourceCreateErrorCategory::RHIResource;
			return FRenderResourceAdmissionScope::GetDeferredAdmissions() != DeferredBefore
				? FShaderCompletion::Pending() : FShaderCompletion::Failure("Mesh pass shader creation failed.");
		}
		Candidate.FragmentLayout = Fragment.ResolveLayout ? Fragment.ResolveLayout(Candidate.ShaderMap) : nullptr;
		Candidate.VertexFactoryPass = MeshPass;
		Candidate.FragmentType = Fragment.Type;
		Candidate.FactoryKey = Factory.GetType().GetStableKey();
		Candidate.LayoutKey = Factory.GetLayoutKey();
		Out = std::move(Candidate);
		return FShaderCompletion{};
	}
	auto FMeshPassProcessor::BuildPSOInitializer(const FMeshPassProcessorBatch& Batch, const FMeshPassShaders& Shaders,
		FRHIVertexDeclaration* Declaration, FGraphicsPipelineStateInitializer& Out) const -> FShaderCompletion
	{
		FSelection Selection;
		const auto Selected = SelectMeshPass(Batch, Selection);
		if (!Selected) return Selected;
		const auto& Factory = Batch.Factory;
		const auto MeshPass = Selection.MeshPass;
		const auto& Fragment = Selection.Fragment;
		const auto& Description = Selection.Description;
		if (!Declaration || !Shaders.ShaderMap || Shaders.ShaderMap.GetIdentity() != Batch.Material.ShaderMap
			|| Shaders.FactoryKey != Factory.GetType().GetStableKey() || Shaders.LayoutKey != Factory.GetLayoutKey()
			|| Shaders.VertexFactoryPass != MeshPass || Shaders.FragmentType != Fragment.Type
			|| !Shaders.VertexShader || !Shaders.FragmentShader || !Shaders.VertexShader->GetRHIShader(false))
			return FShaderCompletion::Failure("Mesh pipeline preparation does not match the selected pass shaders.");
		Out = Description.MakeInitializer(Shaders.VertexShader->GetRHIShader(false), Shaders.FragmentShader,
			Declaration, Shaders.ShaderMap.GetPipelineLayout());
		return FShaderCompletion{};
	}
	auto FMeshPassProcessor::PreparePipeline(const FMeshPassProcessorBatch& Batch, FName Name,
		const FMeshPassShaders& Shaders, FRHIVertexDeclaration* Declaration, FMeshPassPipeline& Out) const -> FShaderCompletion
	{
		FGraphicsPipelineStateInitializer Initializer;
		const auto Result = BuildPSOInitializer(Batch, Shaders, Declaration, Initializer);
		if (!Result) return Result;
		FMeshPassPipeline Candidate;
		static_cast<FMeshPassShaders&>(Candidate) = Shaders;
		Candidate.VertexDeclaration = Declaration;
		Candidate.Description = DescribeMeshDraw(Batch.PipelineState, Batch.Topology, Batch.Material.ShaderMap.BlendMode, Batch.bReversedZ);
		Candidate.Pass = GetPass(); Candidate.bGPUCulling = Batch.bGPUCulling;
		Candidate.PipelineState = FRenderPipelineRequestScope::Graphics(Name, Initializer);
		if (!Candidate.PipelineState) return FShaderCompletion::Failure("Mesh pass pipeline creation failed.");
		Out = std::move(Candidate);
		return FShaderCompletion{};
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
	auto FPassProcessorManager::CreateMeshPassProcessor(EMeshPipelinePass Pass, int32 PSOCollectorIndex)
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
		auto Processor = Create ? Create(PSOCollectorIndex) : nullptr;
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
		auto Prototype = Create(-1);
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

	FMeshPassProcessor::FMeshPassProcessor(int32 InPSOCollectorIndex) : IPSOCollector(InPSOCollectorIndex) {}
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
	auto FMeshPassProcessor::CollectPSOInitializers(const FSceneRenderPreparationConfig& Config,
		const FMaterialResource& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
		const FPSOPrecacheParams& Params, FPSOPrecacheDataArray& Out) -> void
	{
		CheckRenderingThread();
		auto& Prepared = FPreparedMeshPSOCollectorScope::Get();
		const auto Properties = Material.GetStaticProperties();
		const auto& Map = Material.CompiledProgram->GetShaderMap();
		const FMaterialPlanningPassIdentity Identity{
			.ShaderMap = {.RenderLayout = Map.Layout.Identity, .ProgramIdentity = Map.ShaderMapId,
				.BlendMode = Properties.BlendMode, .ShadingModel = Properties.ShadingModel,
				.OpacityMaskThreshold = Properties.OpacityMaskThreshold},
			.bTwoSided = Properties.bTwoSided, .DepthWritePolicy = Properties.DepthWritePolicy};
		const auto Factory = FindMeshVertexFactory(VertexFactoryData.VertexFactoryType->GetStableKey());
		if (!IsEnabled(Config) || !SupportsVertexFactory(*Factory)
			|| !IsMeshRelevant(Identity, Params.Participation, Factory->GetShaderType(MaterialMeshPassGBuffer) != nullptr)) return;
		for (const auto& Variant : Prepared.Variants)
		{
			const FMeshPassProcessorBatch Batch{Identity, *Factory, MakeMeshPipelineState(Identity, Variant.Raster),
				Params.Topology, Params.Participation, true, false, Variant.Raster.bReversedZ};
			FGraphicsPipelineStateInitializer Initializer;
			if (!BuildPSOInitializer(Batch, *Prepared.Shaders, Prepared.Declaration, Initializer))
			{ Prepared.Error = "Mesh pass initializer construction failed."; return; }
			Out.emplace_back(Initializer, Variant.bRequired, Prepared.Priority);
		}
	}

}

namespace Durin
{
	auto FResolvedStaticMeshView::GetMeshDrawCommand(const FPreparedStaticMeshDraw& Draw, RendererPrivate::FMeshPassKey Pass) const
		-> std::shared_ptr<const RendererPrivate::FMeshDrawCommand>
	{
		if (!DrawCommandStorage || Draw.ResolvedIndex >= Draws.size()) return {};
		const auto Found = Draws[Draw.ResolvedIndex].Commands.find(Pass);
		return Found == Draws[Draw.ResolvedIndex].Commands.end() ? nullptr : DrawCommandStorage->GetCommand(Found->second);
	}
}
