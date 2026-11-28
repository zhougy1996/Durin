#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshVertexFactory.h"
#include "SceneInfo.h"
#include "Scene.h"
#include "Renderers/MaterialPSOPrecacheCollector.h"

#include "DynamicRHI.h"
#include "CoreGlobals.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialShaderMap.h"
#include "Renderers/MeshRenderingCommon.h"
#include "Renderers/MeshRendererShared.h"
#include "RenderPipelineCreation.h"
#include "RenderResourcePreparation.h"
#include "RenderingThread.h"
#include "Shader/ShaderMapPreparation.h"

namespace Durin::RendererPrivate
{
	auto FMeshDrawShaderBindings::SetOnCommandList(FRHICommandList& Commands, FShaderBindingState& State, FRHIBuffer* InstanceBuffer, uint32 InstanceDataByteOffset) const -> bool
	{
		if (!Shader || (InstanceBuffer && (InstanceDataByteOffset >= InstanceBuffer->GetSize()
            || InstanceDataByteOffset % sizeof(uint32) != 0))) return false;
		if (State.Shader != Shader) { State.Shader = Shader; State.Parameters.clear(); }
		State.Changed.clear();
		for (auto Parameter : Parameters)
		{
			if (InstanceBuffer && InstanceIdsAllocation
				&& Parameter.SetIndex == InstanceIdsAllocation->first && Parameter.BindingIndex == InstanceIdsAllocation->second
				&& Parameter.Type == ERHIBindingType::StorageBuffer)
			{ Parameter.Resource = InstanceBuffer; Parameter.Offset = InstanceDataByteOffset; Parameter.Size = InstanceBuffer->GetSize() - InstanceDataByteOffset; }
			const auto Found = std::ranges::find_if(State.Parameters, [&](const auto& Previous) {
				return Previous.SetIndex == Parameter.SetIndex && Previous.BindingIndex == Parameter.BindingIndex
					&& Previous.ArrayElement == Parameter.ArrayElement;
			});
			if (Found != State.Parameters.end() && Found->Resource == Parameter.Resource && Found->Type == Parameter.Type
				&& Found->Offset == Parameter.Offset && Found->Size == Parameter.Size && Found->ResourceIndex == Parameter.ResourceIndex) continue;
			State.Changed.push_back(Parameter);
			if (Found == State.Parameters.end()) State.Parameters.push_back(Parameter); else *Found = Parameter;
		}
		if (!State.Changed.empty()) Commands.SetShaderParameters(Shader, State.Changed);
		return true;
	}
	auto FMeshDrawShaderBindings::operator==(const FMeshDrawShaderBindings& Other) const -> bool
	{
		return Shader == Other.Shader && InstanceIdsAllocation == Other.InstanceIdsAllocation
			&& std::ranges::equal(Parameters, Other.Parameters, [](const auto& A, const auto& B) {
			return A.Resource == B.Resource && A.SetIndex == B.SetIndex && A.BindingIndex == B.BindingIndex
				&& A.ArrayElement == B.ArrayElement && A.Type == B.Type && A.Offset == B.Offset && A.Size == B.Size && A.ResourceIndex == B.ResourceIndex;
		});
	}
	namespace
	{
		auto GetMeshPipelineIdentity(const FMeshPassPipelineInitializer& Init) -> FMeshPipelineIdentity
		{
			FMeshPipelineIdentity Key;
			Key.Pass = Init.Pass;
			Key.FactoryKey = Init.FactoryKey;
			Key.LayoutKey = Init.LayoutKey;
			Key.VertexFactoryPass = Init.VertexFactoryPass;
			Key.bHasShaderMap = bool(Init.ShaderMap);
			if (const auto& PSO = Init.PipelineInitializer)
			{
				Key.Description.RenderTargets = PSO->RenderTargetLayout;
				Key.Description.Topology = PSO->PrimitiveTopology;
				if (PSO->RasterizerState) Key.Description.Rasterizer = PSO->RasterizerState->GetInitializer();
				if (PSO->DepthStencilState) Key.Description.Depth = PSO->DepthStencilState->GetInitializer();
				if (PSO->BlendState) Key.Description.ColorBlends = PSO->BlendState->GetInitializer();
				Key.bHasFixedState = PSO->RasterizerState && PSO->DepthStencilState && PSO->BlendState;
				Key.Multisample = PSO->MultisampleState;
				Key.VertexShader = PSO->BoundShaders.VertexShader;
				Key.FragmentShader = PSO->BoundShaders.FragmentShader;
				if (PSO->VertexDeclaration) Key.VertexElements = PSO->VertexDeclaration->GetElements();
				Key.PipelineLayout = PSO->PipelineLayout;
			}
			else
			{
				Key.Description = Init.Description;
				Key.VertexShader = Init.VertexShader ? Init.VertexShader->GetRHIShader(false) : nullptr;
				Key.FragmentShader = Init.FragmentShader;
				if (Init.VertexDeclaration) Key.VertexElements = Init.VertexDeclaration->GetElements();
				if (Init.ShaderMap) Key.PipelineLayout = Init.ShaderMap.GetPipelineLayout();
			}
			return Key;
		}

		// A live command owns its identity. Dynamic commands release theirs with the pass,
		// cached commands retain theirs until removal. Hash collisions always compare by value.
		auto ShareMeshPipelineIdentity(FMeshPipelineIdentity Value) -> std::shared_ptr<const FMeshPipelineIdentity>
		{
			static std::mutex Mutex;
			static std::unordered_multimap<size_t, std::weak_ptr<const FMeshPipelineIdentity>> Entries;
			static size_t NextCleanup = 128;
			const size_t Hash = std::hash<FRHIShader*>{}(Value.VertexShader)
				^ (std::hash<FRHIShader*>{}(Value.FragmentShader) << 1)
				^ (Value.LayoutKey.HashValue >> 1) ^ static_cast<size_t>(Value.Pass);
			std::lock_guard Lock(Mutex);
			const auto [First, End] = Entries.equal_range(Hash);
			for (auto It = First; It != End;)
			{
				auto Existing = It->second.lock();
				if (!Existing) It = Entries.erase(It);
				else if (*Existing == Value) return Existing;
				else ++It;
			}
			if (Entries.size() >= NextCleanup)
			{
				std::erase_if(Entries, [](const auto& Entry) { return Entry.second.expired(); });
				NextCleanup = Entries.size() * 2 + 128;
			}
			auto Identity = std::make_shared<const FMeshPipelineIdentity>(std::move(Value));
			Entries.emplace(Hash, Identity);
			return Identity;
		}
	}
	FMeshPassPipeline::FMeshPassPipeline(FMeshPassPipelineInitializer InInitializer)
		: Initializer(std::move(InInitializer))
		, Identity(ShareMeshPipelineIdentity(GetMeshPipelineIdentity(Initializer)))
	{}
	auto FCachedMeshDrawCommandStorage::MakeKey(const FPreparedStaticMeshDraw& Draw, const FPreparedStaticMeshPrimitive& Primitive,
		const FMeshPassPipeline& Pipeline, const FRHIResource* MaterialUniform) -> FKey
	{
		const auto& Initializer = Pipeline.GetInitializer();
		const auto& Description = *Draw.Command;
		const FStaticMeshCommandVariant Variant{Initializer.Pass, Initializer.Description.Depth.CompareOp,
			Initializer.Description.RenderTargets.NumColorRenderTargets};
		FKey Key;
		if (Primitive.StaticMeshes)
			Key.Mesh = FStaticMeshLocation{Primitive.StaticMeshes, Draw.StaticMeshIndex, Variant};
		else
			Key.Mesh = FDynamicMeshKey{reinterpret_cast<uint64>(Description.GeometryOwner ? static_cast<const void*>(Description.GeometryOwner.get()) : &Description),
				Description.SectionIndex, Primitive.PrimitiveId.Value, Primitive.BatchId, static_cast<uint64>(Variant.Pass),
				static_cast<uint64>(Variant.DepthCompare), Variant.NumColorRenderTargets};
		const auto& Material = Description.Material;
		Key.Bindings = {reinterpret_cast<uint64>(Material.UniformExpressionCache.get()),
			MaterialUniform ? reinterpret_cast<uint64>(MaterialUniform) : Material.Representation.GetRecordId()};
		Key.Pipeline = Pipeline.GetIdentity();
		return Key;
	}
	FCachedMeshDrawCommandStorage::~FCachedMeshDrawCommandStorage()
	{
		if (IsInitialized()) ReleaseResource();
		Reset();
		for (auto* Source : Sources) Source->DrawLists = nullptr;
	}
	auto FCachedMeshDrawCommandStorage::Attach(FPrimitiveSceneInfo& Owner) -> void
	{
		if (Owner.DrawLists == this) return;
		if (Owner.DrawLists) Owner.DrawLists->Detach(Owner);
		Owner.DrawLists = this; Sources.insert(&Owner);
	}
	auto FCachedMeshDrawCommandStorage::RequireWritable() const -> void
	{
		requiref(ReadScope.use_count() == 1, "Scene draw cache mutation during mesh pass recording");
	}
	auto FCachedMeshDrawCommandStorage::RemoveEntry(size_t Index) -> void
	{
		RequireWritable();
		const uint32 Hash = Entries[Index]->StateBucket->Command->GetDynamicInstancingHash();
		Entries[Index]->Info->CommandIndex = INDEX_NONE; Entries[Index].reset();
		if (const auto Found = StateBuckets.find(Hash); Found != StateBuckets.end())
		{
			std::erase_if(Found->second, [](const auto& Bucket) { return Bucket.expired(); });
			if (Found->second.empty()) StateBuckets.erase(Found);
		}
	}
	auto FCachedMeshDrawCommandStorage::RemovePrimitive(FPrimitiveSceneInfo& Owner) -> void
	{
		const auto ReleaseIndex = [&](int32 Index) {
			require(Index >= 0 && static_cast<size_t>(Index) < Entries.size() && Entries[Index]);
			RemoveEntry(Index);
			FreeIndices.push_back(Index);
		};
		for (auto& Relevance : Owner.StaticMeshRelevances)
		{
			for (const auto& Slot : Relevance.CommandInfos) ReleaseIndex(Slot.CommandIndex);
			Relevance.CommandInfos.clear();
		}
		for (const auto& [Key, Index] : Owner.DynamicMeshCommandIndices) ReleaseIndex(Index);
		Owner.DynamicMeshCommandIndices.clear();
	}
	auto FCachedMeshDrawCommandStorage::Detach(FPrimitiveSceneInfo& Owner) -> void
	{ RemovePrimitive(Owner); Sources.erase(&Owner); Owner.DrawLists = nullptr; }
	auto FCachedMeshDrawCommandStorage::Reset() -> void
	{
		RequireWritable();
		MeshPreparation.Reset();
		for (auto* Source : Sources) RemovePrimitive(*Source);
		Entries.clear(); FreeIndices.clear(); StateBuckets.clear(); NextStateBucketId = 0;
	}
	auto FCachedMeshDrawCommandStorage::FindEntry(const FKey& Key, const FPrimitiveSceneInfo& Owner) const -> const FEntry*
	{
		if (Owner.DrawLists != this) return nullptr;
		int32 Index;
		if (const auto* Static = std::get_if<FStaticMeshLocation>(&Key.Mesh))
		{
			if (Static->List != Owner.StaticMeshes || Static->MeshIndex >= Owner.StaticMeshRelevances.size()) return nullptr;
			const auto& Slots = Owner.StaticMeshRelevances[Static->MeshIndex].CommandInfos;
			const auto Slot = std::ranges::find(Slots, Static->Variant, &FStaticMeshBatchRelevance::FCommandSlot::Variant);
			if (Slot == Slots.end()) return nullptr;
			Index = Slot->CommandIndex;
		}
		else
		{
			const auto Found = Owner.DynamicMeshCommandIndices.find(std::get<FDynamicMeshKey>(Key.Mesh));
			if (Found == Owner.DynamicMeshCommandIndices.end()) return nullptr;
			Index = Found->second;
		}
		const auto& Entry = Entries[Index];
		return Entry && Entry->Bindings == Key.Bindings && (Entry->Pipeline == Key.Pipeline || *Entry->Pipeline == *Key.Pipeline) ? Entry.get() : nullptr;
	}
	auto FCachedMeshDrawCommandStorage::FindCachedCommand(const FKey& Key, const FPrimitiveSceneInfo& Owner,
		FResolvedStaticMeshView::FMaterialUniform* Material, const FVertexFactoryInputBinding* Input) const -> FCachedMeshDrawCommand
	{
		const auto* Entry = FindEntry(Key, Owner);
		if (!Entry || (Input && Entry->Input != Input)) return {};
		if (Material) *Material = Entry->Material;
		return {Entry->StateBucket->Command, Entry->Info};
	}
	auto FCachedMeshDrawCommandStorage::Publish(const FKey& Key, FMeshDrawCommand Command, FPrimitiveSceneInfo& Owner,
		std::shared_ptr<const FMeshPassPipeline> Pipeline,
		const FVertexFactoryInputBinding* Input, FResolvedStaticMeshView::FMaterialUniform Material) -> FCachedMeshDrawCommand
	{
		require(Command.bFinalized && Command.bCached && Pipeline && Command.Pipeline == Pipeline.get());
		RequireWritable();
		const auto* Static = std::get_if<FStaticMeshLocation>(&Key.Mesh);
		if (Static && (Static->List != Owner.StaticMeshes || Static->MeshIndex >= Owner.StaticMeshRelevances.size())) return {};
		Attach(Owner);
		if (IsInRenderingThread() && !IsInitialized()) InitResource(FRHICommandListImmediate::Get());
		int32* CommandIndex;
		if (Static)
		{
			auto& Slots = Owner.StaticMeshRelevances[Static->MeshIndex].CommandInfos;
			const auto Slot = std::ranges::find(Slots, Static->Variant, &FStaticMeshBatchRelevance::FCommandSlot::Variant);
			CommandIndex = Slot == Slots.end() ? &Slots.emplace_back(Static->Variant, INDEX_NONE).CommandIndex : &Slot->CommandIndex;
		}
		else
			CommandIndex = &Owner.DynamicMeshCommandIndices.try_emplace(std::get<FDynamicMeshKey>(Key.Mesh), INDEX_NONE).first->second;
		size_t Index;
		if (*CommandIndex == INDEX_NONE)
		{
			if (FreeIndices.empty()) { Index = Entries.size(); Entries.emplace_back(); }
			else { Index = FreeIndices.back(); FreeIndices.pop_back(); }
			require(Index < static_cast<size_t>(INT32_MAX));
			*CommandIndex = static_cast<int32>(Index);
		}
		else
		{
			Index = *CommandIndex;
			RemoveEntry(Index);
		}
		auto Info = std::make_shared<FCachedMeshDrawCommandInfo>();
		Info->Storage = this;
		Info->CommandIndex = *CommandIndex;
		Info->MeshPass = Command.Pipeline->GetPassKey();
		for (auto& Range : Material.CollectionUniforms) Range.ResourceOwner = {};
		auto Entry = std::make_unique<FEntry>();
		Entry->Info = std::move(Info);
		const auto Factory = FindMeshVertexFactory(Command.Pipeline->GetInitializer().FactoryKey);
		const bool bShare = Factory && Factory->SupportsGPUScene()
			&& std::visit([](const auto& Args) { return Args.InstanceCount == 1; }, Command.DrawParameters)
			&& std::ranges::none_of(Command.Pipeline->GetInitializer().VertexDeclaration->GetElements(),
				[](const auto& Element) { return Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance; });
		const uint32 Hash = Command.GetDynamicInstancingHash();
		if (bShare)
		{
			auto& Buckets = StateBuckets[Hash];
			for (const auto& Weak : Buckets)
				if (const auto Bucket = Weak.lock(); Bucket && Command.MatchesForDynamicInstancing(*Bucket->Command))
				{ Entry->StateBucket = Bucket; break; }
		}
		if (!Entry->StateBucket)
		{
			auto Bucket = std::make_shared<FStateBucket>();
			Bucket->Command = std::make_shared<const FMeshDrawCommand>(std::move(Command));
			Bucket->Pipeline = Pipeline;
			require(NextStateBucketId < INT32_MAX);
			Bucket->Id = NextStateBucketId++; Entry->StateBucket = Bucket;
			if (bShare) StateBuckets[Hash].push_back(Bucket);
		}
		Entry->Info->StateBucketId = Entry->StateBucket->Id;
		Entry->Bindings = Key.Bindings; Entry->Pipeline = Key.Pipeline; Entry->Input = Input; Entry->Material = std::move(Material);
		// Only the state bucket retains its canonical pipeline. Lookup uses value identity.
		Entries[Index] = std::move(Entry);
		return {Entries[Index]->StateBucket->Command, Entries[Index]->Info};
	}
	auto FCachedMeshDrawCommandStorage::ReleaseRHI() -> void { Reset(); }
	auto FCachedMeshDrawCommandStorage::ReleaseRHIForAllResources() -> void
	{
		std::vector<FCachedMeshDrawCommandStorage*> Storages;
		VisitInitializedRenderResources([&](FRenderResource& Resource) {
			if (auto* Storage = dynamic_cast<FCachedMeshDrawCommandStorage*>(&Resource)) Storages.push_back(Storage);
		});
		for (auto* Storage : Storages) Storage->ReleaseResource();
	}
	auto FCachedMeshDrawCommandStorage::InvalidateMeshPreparationForAllResources(bool bUpdateStaticMeshElements) -> void
	{
		VisitInitializedRenderResources([bUpdateStaticMeshElements](FRenderResource& Resource) {
			if (auto* Storage = dynamic_cast<FCachedMeshDrawCommandStorage*>(&Resource))
			{
				Storage->MeshPreparation.Reset();
				Storage->bNeedsStaticMeshElementUpdate |= bUpdateStaticMeshElements;
			}
		});
	}
	FDynamicMeshDrawCommandStorage::FDynamicMeshDrawCommandStorage() : Data(std::make_shared<FData>())
	{ Data->Frame = GRenderFrameCounterRenderThread; }
	auto FParallelMeshDrawCommandPass::AddCachedMeshDrawCommand(FCachedMeshDrawCommand Cached,
		std::shared_ptr<const FMeshPassBindings> Bindings,
		std::array<float, 3> Bias, uint32 InstanceIdOffset, uint32 SourceDrawIndex) -> void
	{
		const auto& [Command, Info] = Cached;
		require(Cached && Command->bCached && Info->Storage && Info->CommandIndex != INDEX_NONE && Bindings);
		const auto Key = Info->MeshPass;
		require(Command->Pipeline && Command->Pipeline->GetPassKey() == Key);
		require(Key == Pass && !bFinalized && !CacheReadScope);
		auto& List = VisibleMeshDrawCommands;
		List.push_back({.Command = Command.get(), .Pass = Bindings.get(), .DepthBias = Bias,
			.PrimitiveIdBufferOffset = InstanceIdOffset,
			.SourceDrawIndex = SourceDrawIndex == INDEX_NONE_U32 ? static_cast<uint32>(List.size()) : SourceDrawIndex});
		RetainedCachedDraws.push_back({std::move(Cached), std::move(Bindings)});
	}
	auto FParallelMeshDrawCommandPass::BeginRecording() -> bool
	{
		if (CacheReadScope || RetainedCachedDraws.empty()) return true;
		const auto* Storage = RetainedCachedDraws.front().Cached.Info->Storage;
		for (const auto& Draw : RetainedCachedDraws)
			if (Draw.Cached.Info->CommandIndex == INDEX_NONE || Draw.Cached.Info->Storage != Storage) return false;
		CacheReadScope = Storage->ReadScope;
		return true;
	}
	auto FParallelMeshDrawCommandPass::AddMeshDrawCommand(std::shared_ptr<const FMeshDrawCommand> Command, uint32 SourceDrawIndex) -> void
	{
		require(Command && !Command->bCached && Command->Pipeline && Command->Pipeline->GetPassKey() == Pass && !bFinalized && !CacheReadScope);
		if (!DynamicStorageOwner) DynamicStorageOwner = Command;
		require(!DynamicStorageOwner.owner_before(Command) && !Command.owner_before(DynamicStorageOwner));
		VisibleMeshDrawCommands.push_back({.Command = Command.get(), .Pass = &Command->PassBindings, .DepthBias = Command->DepthBias,
			.PrimitiveIdBufferOffset = Command->InstanceIdOffset, .SourceDrawIndex = SourceDrawIndex});
	}
	auto FParallelMeshDrawCommandPass::GetVisibleForSource(uint32 SourceDrawIndex) const -> const FVisibleMeshDrawCommand*
	{
		if (RenderFrame != GRenderFrameCounterRenderThread || SourceDrawIndex >= SourceCommandIndices.size()) return nullptr;
		const auto Index = SourceCommandIndices[SourceDrawIndex];
		if (Index == INDEX_NONE_U32) return nullptr;
		return &VisibleMeshDrawCommands[Index];
	}
	auto FCachedPassMeshDrawListContext::FinalizeCommand(const FMeshDrawCommandBuildInput& Input, FMeshDrawCommand& Command) -> void
	{
		require(!Command.bFinalized && Command.bCached && Command.Pipeline && Command.Pipeline->GetPassKey() == Pass);
		Command.bFinalized = true;
		const auto Owner = Input.Primitive.SceneInfo.Resolve(); require(Owner);
		CommandInfo = Storage.Publish(FCachedMeshDrawCommandStorage::MakeKey(Input.Draw, Input.Primitive, *Input.Pipeline, Input.Material.Uniform.Buffer), std::move(Command), *Owner, Input.Pipeline, Input.Primitive.CollectedBinding.get(), Input.Material);
	}
	auto FDynamicPassMeshDrawListContext::AddCommand(FMeshDrawCommand& Initializer) -> FMeshDrawCommand&
	{
		require(Storage.Data->Frame == GRenderFrameCounterRenderThread);
		auto& Command = Storage.Data->Commands.emplace_back(Initializer);
		Command.bFinalized = false;
		return Command;
	}
	auto FDynamicPassMeshDrawListContext::FinalizeCommand(const FMeshDrawCommandBuildInput& Input, FMeshDrawCommand& Command) -> void
	{
		require(Storage.Data->Frame == GRenderFrameCounterRenderThread);
		require(!Command.bFinalized && !Command.bCached && Command.Pipeline && Command.Pipeline->GetPassKey() == Pass.GetPass());
		Command.bFinalized = true;
		Pass.AddMeshDrawCommand(std::shared_ptr<const FMeshDrawCommand>(Storage.Data, &Command), Input.Draw.ResolvedIndex);
	}

	auto FMeshDrawCommand::SetDrawParameters(const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Draw) -> bool
	{
		require(!bFinalized);
		if (!Draw.Command || !Primitive.CollectedBinding || !Primitive.CollectedBinding->Declaration
			|| (Pipeline && Draw.Command->Geometry.Topology != Pipeline->GetInitializer().Description.Topology)
			|| !Draw.Command->Vertices.IsValid()
			|| (Draw.Command->Geometry.bIndexed && (!Draw.Command->Indices.IsValid()
				|| Draw.Command->Indices.Range.ByteOffset > std::numeric_limits<uint32>::max()))
			|| Draw.Command->Geometry.Validate(Draw.Command->Vertices.Range, Draw.Command->Indices.Range) != EGeometrySubmissionOutcome::Submitted
			|| Primitive.CollectedBinding->ValidateInputs(Draw.Command->Geometry) != EGeometrySubmissionOutcome::Submitted) return false;
		VertexStreams.clear();
		for (const auto& Stream : Primitive.CollectedBinding->Streams) VertexStreams.push_back({Stream.StreamIndex, Stream.VertexBuffer.GetReference(), Stream.Offset, Stream.Stride});
		IndexBuffer = Draw.Command->Geometry.bIndexed ? Draw.Command->Indices.Buffer.GetReference() : nullptr;
		IndexBufferOffset = IndexBuffer ? static_cast<uint32>(Draw.Command->Indices.Range.ByteOffset) : 0;
		if (IndexBuffer) DrawParameters = Draw.Command->Geometry.GetIndexedDrawArguments();
		else DrawParameters = Draw.Command->Geometry.GetDrawArguments();
		IndirectArgs = {}; IndirectArgsOffset = 0;
		return true;
	}

	auto FMeshDrawCommand::SubmitDraw(FRHICommandList& Commands, FMeshDrawCommandStateCache* StateCache) const -> bool
	{
		if (bCached) return false;
		return SubmitVisible(Commands, PassBindings.Vertex, PassBindings.Fragment, DepthBias, StateCache, {.PrimitiveIdOffset = InstanceIdOffset});
	}

	auto FMeshDrawCommand::SubmitVisible(FRHICommandList& Commands, const FMeshDrawShaderBindings& VertexPass, const FMeshDrawShaderBindings& FragmentPass,
		const std::array<float, 3>& Bias,
		FMeshDrawCommandStateCache* StateCache, const FMeshDrawCommandSceneArgs& SceneArgs) const -> bool
	{
		if (!bFinalized || !Commands.IsInsideRenderPass() || !Pipeline
			|| !std::ranges::all_of(Bias, [](float Value) { return std::isfinite(Value); })) return false;
        if (SceneArgs.PrimitiveIdsBuffer && (SceneArgs.InstanceDataByteOffset % sizeof(uint32) != 0
            || SceneArgs.InstanceDataByteOffset >= SceneArgs.PrimitiveIdsBuffer->GetSize())) return false;
		FMeshDrawCommandStateCache Local;
		auto& Cache = StateCache ? *StateCache : Local;
		const auto& Initializer = Pipeline->GetInitializer().PipelineInitializer;
		if (!Initializer || !VertexBindings || !VertexPass
			|| (Pipeline->GetInitializer().FragmentLayout && (!FragmentBindings || !FragmentPass))) return false;
		if (Cache.PipelineId != Pipeline->GetIdentity())
		{
			SetGraphicsPipelineState(Commands, *Initializer);
			Cache.PipelineId = Pipeline->GetIdentity();
			Cache.Vertex.Shader = Cache.Fragment.Shader = nullptr;
			Cache.Vertex.Parameters.clear(); Cache.Fragment.Parameters.clear();
			Cache.DepthBias.reset();
		}
		if (!SceneArgs.VertexPushConstants.empty())
			Commands.PushConstants(EShaderStageFlags::Vertex, 0, static_cast<uint32>(SceneArgs.VertexPushConstants.size()), SceneArgs.VertexPushConstants.data());
		VertexBindings.SetOnCommandList(Commands, Cache.Vertex);
		if (VertexPass) VertexPass.SetOnCommandList(Commands, Cache.Vertex, SceneArgs.PrimitiveIdsBuffer, SceneArgs.InstanceDataByteOffset);
		if (FragmentBindings) FragmentBindings.SetOnCommandList(Commands, Cache.Fragment);
		if (FragmentPass) FragmentPass.SetOnCommandList(Commands, Cache.Fragment, SceneArgs.PrimitiveIdsBuffer, SceneArgs.InstanceDataByteOffset);
		for (const auto& Stream : VertexStreams)
		{
			const auto Found = std::ranges::find(Cache.VertexStreams, Stream.StreamIndex, &FMeshDrawVertexStream::StreamIndex);
			if (Found != Cache.VertexStreams.end() && *Found == Stream) continue;
			Commands.BindVertexBuffer(Stream.StreamIndex, Stream.VertexBuffer, Stream.Offset);
			if (Found == Cache.VertexStreams.end()) Cache.VertexStreams.push_back(Stream); else *Found = Stream;
		}
		if (IndexBuffer && (Cache.IndexBuffer != IndexBuffer || Cache.IndexBufferOffset != IndexBufferOffset))
		{
			Commands.BindIndexBuffer(IndexBuffer, IndexBufferOffset);
			Cache.IndexBuffer = IndexBuffer; Cache.IndexBufferOffset = IndexBufferOffset;
		}
		if (Initializer->RasterizerState->GetInitializer().bEnableDepthBias && Cache.DepthBias != Bias)
		{ Commands.SetDepthBias(Bias[0], Bias[2], Bias[1]); Cache.DepthBias = Bias; }
		const auto& Geometry = *this;
		const auto ArgsBuffer = SceneArgs.IndirectArgsBuffer ? SceneArgs.IndirectArgsBuffer : IndirectArgs;
		const auto ArgsOffset = SceneArgs.IndirectArgsBuffer ? SceneArgs.IndirectArgsByteOffset : IndirectArgsOffset;
		if (ArgsBuffer)
		{
			if (Geometry.IndexBuffer) Commands.DrawIndexedIndirect(ArgsBuffer, ArgsOffset);
			else Commands.DrawIndirect(ArgsBuffer, ArgsOffset);
		}
		else if (const auto* Indexed = std::get_if<FRHIDrawIndexedArguments>(&Geometry.DrawParameters))
		{ auto Args = *Indexed; if (SceneArgs.PrimitiveIdOffset != INDEX_NONE_U32) Args.FirstInstance = SceneArgs.PrimitiveIdOffset; if (SceneArgs.NumInstances) Args.InstanceCount = SceneArgs.NumInstances; Commands.DrawIndexed(Args); }
		else { auto Args = std::get<FRHIDrawArguments>(Geometry.DrawParameters); if (SceneArgs.PrimitiveIdOffset != INDEX_NONE_U32) Args.FirstInstance = SceneArgs.PrimitiveIdOffset; if (SceneArgs.NumInstances) Args.InstanceCount = SceneArgs.NumInstances; Commands.Draw(Args); }
		++Cache.DrawCalls;
		if (PSOPrecacheValidation::IsEnabled()) {
			const auto Index = FPassProcessorManager::GetPSOCollectorIndex(EShadingPath::Deferred, Pipeline->GetInitializer().Pass);
			const auto Factory = FindMeshVertexFactory(Pipeline->GetInitializer().FactoryKey);
			PSOPrecacheValidation::RecordUse(*Initializer, Index, Factory ? &Factory->GetType() : nullptr);
		}
		return true;
	}

	auto FMeshPassProcessor::SelectMeshPass(const FMeshPassProcessorBatch& Batch, FSelection& Out) const -> FShaderCompletion
	{
		CheckRenderingThread();
		const auto& Factory = Batch.Factory;
		const auto MeshPass = GetVertexFactoryPass();
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
	auto MakeMeshPassProcessorBatch(const FMeshDrawCommandBuildInput& Input,
		const FMeshVertexFactoryImplementation& Factory) -> FMeshPassProcessorBatch
	{
		const auto& Draw = *Input.Draw.Command;
		const auto& Key = Draw.PipelineKey;
		const auto& Pipeline = Input.Pipeline->GetInitializer();
		const bool bModelData = Pipeline.Pass == EMeshPipelinePass::GBuffer && Pipeline.Description.RenderTargets.NumColorRenderTargets == 5;
		return {Draw.Material.PlanningPassIdentity, Factory, {Key.Rasterizer, Key.Depth, Key.ColorBlend, bModelData},
			Key.Topology, {}, Draw.bSupportsGBuffer, Key.Depth.CompareOp == ERHIDepthCompareOp::GreaterOrEqual};
	}
	auto FMeshPassProcessor::BuildMeshDrawCommands(const FMeshPassProcessorBatch& Batch, const FMeshDrawCommandBuildInput& Input,
		std::shared_ptr<const FMeshDrawCommand> Existing) const -> FShaderCompletion
	{
		FSelection Selection;
		const auto Selected = SelectMeshPass(Batch, Selection);
		if (!Selected) return Selected;
		if (!DrawListContext) return FShaderCompletion::Failure("Mesh command build requires a local draw-list context.");
		auto& DrawContext = *DrawListContext;
		auto& Cache = DrawContext.Cache;
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
			|| Pipeline->GetInitializer().Pass != GetPass()
			|| Pipeline->GetInitializer().Description != Description
			|| !Pipeline->GetInitializer().ShaderMap || Pipeline->GetInitializer().ShaderMap.GetIdentity() != Batch.Material.ShaderMap
			|| Pipeline->GetInitializer().VertexFactoryPass != MeshPass || Pipeline->GetInitializer().FragmentType != Fragment.Type
			|| Pipeline->GetInitializer().FactoryKey != Key.FactoryKey || Pipeline->GetInitializer().LayoutKey != Key.LayoutKey
			|| (DrawContext.IsCached() && Input.HitProxy.Buffer))
		{ return FShaderCompletion::Failure("Mesh command binding context does not match the selected pass and admitted batch."); }
		if (!Pipeline->GetInitializer().PipelineInitializer || !Pipeline->GetInitializer().VertexShader || !Pipeline->GetInitializer().FragmentShader)
		{ return FShaderCompletion::Failure("Mesh command binding requires prepared native resources."); }
		FMeshDrawCommand Initializer;
		auto* Command = &DrawContext.AddCommand(Initializer);
		Command->Pipeline = Pipeline.get(); Command->bCached = DrawContext.IsCached();
		Command->InstanceIdOffset = Command->bCached ? INDEX_NONE_U32 : Input.InstanceIdOffset;
		if (!Existing && !Command->SetDrawParameters(Primitive, Draw))
		{ return FShaderCompletion::Failure("Mesh command geometry finalization failed."); }
		const auto Raster = GetPass() == EMeshPipelinePass::ShadowDepth
			? MakeShadowRasterizerState(Batch.PipelineState.Rasterizer) : Pipeline->GetInitializer().Description.Rasterizer;
		Command->DepthBias = {Raster.DepthBiasConstantFactor, Raster.DepthBiasSlopeFactor, Raster.DepthBiasClamp};
		if (!Command->bCached && GetPass() == EMeshPipelinePass::ShadowDepth && Draw.RasterBias) Command->DepthBias = *Draw.RasterBias;
		if (Existing)
		{
			if (DrawContext.IsCached() || !Existing->bFinalized || !Existing->bCached || Existing->Pipeline != Pipeline.get() || Input.HitProxy.Buffer)
			{ return FShaderCompletion::Failure("Cached mesh command does not match the selected pass."); }
			Command->VertexStreams = Existing->VertexStreams; Command->IndexBuffer = Existing->IndexBuffer;
			Command->IndexBufferOffset = Existing->IndexBufferOffset; Command->DrawParameters = Existing->DrawParameters;
			Command->VertexBindings = Existing->VertexBindings;
			Command->FragmentBindings = Existing->FragmentBindings;
		}
		const auto& Vertex = Pipeline->GetInitializer().VertexShader;
		if (!Command->VertexBindings)
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
		if (Pipeline->GetInitializer().FragmentLayout)
		{
			const auto& Layout = *Pipeline->GetInitializer().FragmentLayout;
			const bool UsesPrimitive = std::ranges::any_of(Layout.GetEntries(), [](const auto& Entry) { return Entry.OwnerName == "Primitive"; });
			const auto FragmentKey = std::make_tuple(Draw.MaterialUniformIndex, BindingPass,
				UsesPrimitive ? Draw.PrimitiveIndex : std::numeric_limits<uint32>::max(), Pipeline->GetInitializer().FragmentShader.GetReference());
			if (Command->FragmentBindings)
			{
				const auto [Found, Inserted] = Cache.Fragments.try_emplace(FragmentKey, Command->FragmentBindings);
				Command->FragmentBindings = Found->second;
			}
			else if (const auto Found = Cache.Fragments.find(FragmentKey); !Input.HitProxy.Buffer && Found != Cache.Fragments.end())
			{ Command->FragmentBindings = Found->second; ++Cache.SurfaceBindingBatchReuses; }
			else
			{
				FMeshDrawShaderBindings Bindings;
				if (!PrepareCompiledSurfaceMaterial(Pipeline->GetInitializer().FragmentShader, Layout.ForMaterialParameters(true), Input.Material.Surface,
					Input.Material.Uniform, {}, Input.HitProxy, {}, Input.PrimitiveUniform, Input.Material.CollectionUniforms, Bindings))
				{ return FShaderCompletion::Failure("Mesh stable fragment binding collection failed."); }
				Command->FragmentBindings = std::move(Bindings);
				if (!Input.HitProxy.Buffer) Cache.Fragments.emplace(FragmentKey, Command->FragmentBindings);
				++Cache.PreparedSurfaceBindingBatches;
			}
		}
		if (!DrawContext.IsCached())
		{
			const auto Result = PreparePassBindings(Input, Cache, Command->PassBindings);
			if (!Result) return Result;
		}
		DrawContext.FinalizeCommand(Input, *Command); return FShaderCompletion{};
	}
	auto FMeshPassProcessor::PreparePassBindings(const FMeshDrawCommandBuildInput& Input,
		FMeshDrawCommandBindingCache& Cache, FMeshPassBindings& Out) const -> FShaderCompletion
	{
		const auto& Pipeline = Input.Pipeline;
		const auto BindingPass = GetPass() == EMeshPipelinePass::HybridRetainedForward || GetPass() == EMeshPipelinePass::SortedTranslucency
			? EMeshPipelinePass::Forward : GetPass();
		auto Collect = [&](FRHIShader* Shader, const FCompiledSurfaceBindingLayout& Layout,
			FMeshDrawShaderBindings& Bindings) -> bool {
			const auto Key = std::make_tuple(BindingPass, Shader, Input.GPUScene.GPUSceneInstanceIds.Buffer);
			if (!Input.HitProxy.Buffer)
				if (const auto Found = Cache.Passes.find(Key); Found != Cache.Passes.end()) { Bindings = Found->second; return true; }
			FMeshDrawShaderBindings Prepared;
			auto PassLayout = Layout.ForMaterialParameters(false);
			if (GetPass() == EMeshPipelinePass::SingleLayerWater) PassLayout = PassLayout.WithoutWaterInputs();
			if (!PrepareCompiledSurfaceMaterial(Shader, PassLayout, Input.Material.Surface, {}, Input.Lighting,
				Input.HitProxy, Input.ViewUniform, {}, {}, Prepared, Input.LocalLights, Input.GPUScene)) return false;
			Bindings = std::move(Prepared);
			if (!Input.HitProxy.Buffer) Cache.Passes.emplace(Key, Bindings);
			return true;
		};
		{
			const auto& Vertex = Pipeline->GetInitializer().VertexShader;
			const auto Found = Cache.Passes.find({BindingPass, Vertex->GetRHIShader(false), Input.GPUScene.GPUSceneInstanceIds.Buffer});
			if (!Input.HitProxy.Buffer && Found != Cache.Passes.end()) Out.Vertex = Found->second;
			else
			{
				if (!Vertex->GetReflection()) return FShaderCompletion::Failure("Mesh vertex reflection is unavailable.");
                if (!Collect(Vertex->GetRHIShader(false), FCompiledSurfaceBindingLayout(Vertex->GetAcceptedParameterBindings()), Out.Vertex))
					return FShaderCompletion::Failure("Mesh view vertex binding collection failed.");
			}
		}
		if (Pipeline->GetInitializer().FragmentLayout && !Collect(Pipeline->GetInitializer().FragmentShader, *Pipeline->GetInitializer().FragmentLayout, Out.Fragment))
			return FShaderCompletion::Failure("Mesh view fragment binding collection failed.");
		return FShaderCompletion{};
	}
	auto FMeshPassProcessor::PrepareShaders(const FMeshPassProcessorBatch& Batch,
		const FMaterialProgram* Program, const FShaderCompileOptions& Options, FMeshPassShaders& Out,
		ERenderResourceCreateErrorCategory* FailureCategory) const -> FShaderMapQueryResult
	{
		if (FailureCategory) *FailureCategory = ERenderResourceCreateErrorCategory::InvalidConfiguration;
		FSelection Selection;
		const auto Selected = SelectMeshPass(Batch, Selection);
		if (!Selected) return {Selected.Status == EShaderBuildStatus::Canceled
			? ERenderResourceAvailability::Canceled : ERenderResourceAvailability::Failed, Selected.Error};
		const auto& Factory = Batch.Factory;
		const auto MeshPass = Selection.MeshPass;
		const auto& Fragment = Selection.Fragment;
		FMeshPassShaders Candidate;
		if (FailureCategory) *FailureCategory = ERenderResourceCreateErrorCategory::ShaderCompile;
		auto Result = InitializeMaterialShaderMap(*Factory.GetShaderType(MeshPass), *Fragment.Type,
			Factory.GetType(), MeshPass, Batch.Material.ShaderMap,
			Program, Options, Candidate.ShaderMap);
		if (!Result) return Result;

		Candidate.VertexShader = Factory.Resolve(Candidate.ShaderMap, MeshPass);
		Candidate.FragmentShader = Fragment.Resolve(Candidate.ShaderMap);
		if (!Candidate.VertexShader || !Candidate.VertexShader->GetRHIShader(false) || !Candidate.FragmentShader)
		{
			if (FailureCategory) *FailureCategory = ERenderResourceCreateErrorCategory::RHIResource;
			return {ERenderResourceAvailability::Failed, "Mesh pass shader creation failed."};
		}
		Candidate.FragmentLayout = Fragment.ResolveLayout ? Fragment.ResolveLayout(Candidate.ShaderMap) : nullptr;
		Candidate.VertexFactoryPass = MeshPass;
		Candidate.FragmentType = Fragment.Type;
		Candidate.FactoryKey = Factory.GetType().GetStableKey();
		Candidate.LayoutKey = Factory.GetLayoutKey();
		Out = std::move(Candidate);
		return {};
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
	auto FMeshPassProcessor::PrecachePipelineIfRequested(const FGraphicsPipelineStateInitializer& Initializer) -> FShaderCompletion
	{
		if (!FRenderPipelinePreparationBatch::ShouldPrecachePipelines()) return {};
		auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Initializer);
		if (!Request) return FShaderCompletion::Failure("Mesh pipeline precache request was rejected.");
		FRenderPipelinePreparationBatch::Add(Request);
		const auto Status = Request->GetState();
		if (Status == ERHIPipelineState::Failed || Status == ERHIPipelineState::Canceled)
			return FShaderCompletion::Failure("Mesh pipeline precache did not complete successfully.");
		return {};
	}
	auto FMeshPassProcessor::ResolvePipeline(const FMeshPassProcessorBatch& Batch, FCachedMeshPassShaders& Shaders,
		FRHIVertexDeclaration* Declaration) const -> std::shared_ptr<const FMeshPassPipeline>
	{
		if (!Declaration) return nullptr;
		const auto Description = DescribeMeshDraw(Batch.PipelineState, Batch.Topology, Batch.Material.ShaderMap.BlendMode, Batch.bReversedZ);
		for (const auto& Weak : Shaders.Pipelines)
		{
			auto Pipeline = Weak.lock();
			if (!Pipeline) continue;
			const auto& Cached = Pipeline->GetInitializer();
			if (Cached.Pass != GetPass() || Cached.Description != Description) continue;
			if (Cached.VertexDeclaration != Declaration
				&& Cached.VertexDeclaration->GetElements() != Declaration->GetElements()) continue;
			if (!PrecachePipelineIfRequested(*Cached.PipelineInitializer)) return nullptr;
			return Pipeline;
		}
		std::erase_if(Shaders.Pipelines, [](const auto& Weak) { return Weak.expired(); });
		std::shared_ptr<const FMeshPassPipeline> Pipeline;
		if (!PreparePipeline(Batch, Shaders, Declaration, Pipeline)) return nullptr;
		Shaders.Pipelines.emplace_back(Pipeline);
		return Pipeline;
	}
	auto FMeshPassProcessor::PreparePipeline(const FMeshPassProcessorBatch& Batch,
		const FMeshPassShaders& Shaders, FRHIVertexDeclaration* Declaration, std::shared_ptr<const FMeshPassPipeline>& Out) const -> FShaderCompletion
	{
		FGraphicsPipelineStateInitializer Initializer;
		const auto Result = BuildPSOInitializer(Batch, Shaders, Declaration, Initializer);
		if (!Result) return Result;
		if (const auto Precache = PrecachePipelineIfRequested(Initializer); !Precache) return Precache;
		FMeshPassPipelineInitializer Candidate;
		static_cast<FMeshPassShaders&>(Candidate) = Shaders;
		Candidate.VertexDeclaration = Declaration;
		Candidate.Description = DescribeMeshDraw(Batch.PipelineState, Batch.Topology, Batch.Material.ShaderMap.BlendMode, Batch.bReversedZ);
		Candidate.Pass = GetPass();
		Candidate.PipelineInitializer = std::move(Initializer);
		Out = std::make_shared<const FMeshPassPipeline>(std::move(Candidate));
		return FShaderCompletion{};
	}

	namespace
	{
		constexpr size_t MeshPassSlotCount = static_cast<size_t>(EMeshPipelinePass::HybridSky) + 1;
		struct FPassProcessorEntry
		{
			int32 PSOCollectorIndex = -1;
			EMeshPassFlags Flags = EMeshPassFlags::None;
			PassProcessorCreateFunction Create = nullptr;
			std::unique_ptr<FMeshPassProcessor> Prototype;
		};
		struct FPassProcessorRegistry
		{
			std::mutex Mutex;
			std::array<std::array<FPassProcessorEntry, MeshPassSlotCount>, static_cast<size_t>(EShadingPath::Num)> Entries;
		};
		auto PassProcessorRegistry() -> FPassProcessorRegistry&
		{
			static FPassProcessorRegistry Registry;
			return Registry;
		}
		auto IsValidMeshPass(EShadingPath ShadingPath, EMeshPipelinePass Pass) -> bool
		{ return ShadingPath < EShadingPath::Num && static_cast<size_t>(Pass) < MeshPassSlotCount; }
	}

	FRegisterPassProcessorCreateFunction::FRegisterPassProcessorCreateFunction(
		PassProcessorCreateFunction InCreateFunction, EShadingPath InShadingPath, EMeshPipelinePass InPass, EMeshPassFlags Flags, int32 PSOCollectorIndex)
		: CreateFunction(InCreateFunction), ShadingPath(InShadingPath), Pass(InPass)
	{
		if (!CreateFunction || !IsValidMeshPass(ShadingPath, Pass)) return;
		auto& Registry = PassProcessorRegistry();
		std::lock_guard Lock(Registry.Mutex);
		auto& Entry = Registry.Entries[static_cast<size_t>(ShadingPath)][static_cast<size_t>(Pass)];
		if (Entry.Create) return;
		Entry.Flags = Flags;
		Entry.PSOCollectorIndex = PSOCollectorIndex;
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
			auto& Entry = Registry.Entries[static_cast<size_t>(ShadingPath)][static_cast<size_t>(Pass)];
			if (Entry.Create != CreateFunction) return;
			Retired = std::move(Entry.Prototype);
			Entry.Create = nullptr;
			Entry.Flags = EMeshPassFlags::None;
			Entry.PSOCollectorIndex = -1;
		}
	}
	auto FPassProcessorManager::GetPassFlags(EShadingPath ShadingPath, EMeshPipelinePass Pass) -> EMeshPassFlags
	{
		if (!IsValidMeshPass(ShadingPath, Pass)) return EMeshPassFlags::None;
		auto& Registry = PassProcessorRegistry();
		std::lock_guard Lock(Registry.Mutex);
		return Registry.Entries[static_cast<size_t>(ShadingPath)][static_cast<size_t>(Pass)].Flags;
	}
	auto FPassProcessorManager::GetPSOCollectorIndex(EShadingPath ShadingPath, EMeshPipelinePass Pass) -> int32
	{
		if (!IsValidMeshPass(ShadingPath, Pass)) return -1;
		auto& Registry = PassProcessorRegistry();
		std::lock_guard Lock(Registry.Mutex);
		return Registry.Entries[static_cast<size_t>(ShadingPath)][static_cast<size_t>(Pass)].PSOCollectorIndex;
	}
	auto FPassProcessorManager::IsRegistered(EShadingPath ShadingPath, EMeshPipelinePass Pass) -> bool
	{
		if (!IsValidMeshPass(ShadingPath, Pass)) return false;
		auto& Registry = PassProcessorRegistry();
		std::lock_guard Lock(Registry.Mutex);
		return Registry.Entries[static_cast<size_t>(ShadingPath)][static_cast<size_t>(Pass)].Create != nullptr;
	}
	auto FPassProcessorManager::GetRegisteredPasses(EShadingPath ShadingPath) -> std::vector<EMeshPipelinePass>
	{
		auto& Registry = PassProcessorRegistry();
		std::lock_guard Lock(Registry.Mutex);
		std::vector<EMeshPipelinePass> Passes;
		if (ShadingPath >= EShadingPath::Num) return Passes;
		for (size_t Index = 0; Index < MeshPassSlotCount; ++Index)
			if (Registry.Entries[static_cast<size_t>(ShadingPath)][Index].Create) Passes.push_back(static_cast<EMeshPipelinePass>(Index));
		return Passes;
	}
	auto FPassProcessorManager::CreateMeshPassProcessor(EShadingPath ShadingPath, EMeshPipelinePass Pass, FMeshPassDrawListContext* Context)
		-> std::unique_ptr<FMeshPassProcessor>
	{
		if (!IsValidMeshPass(ShadingPath, Pass)) return {};
		PassProcessorCreateFunction Create = nullptr;
		int32 PSOCollectorIndex = -1;
		{
			auto& Registry = PassProcessorRegistry();
			std::lock_guard Lock(Registry.Mutex);
			const auto& Entry = Registry.Entries[static_cast<size_t>(ShadingPath)][static_cast<size_t>(Pass)];
			Create = Entry.Create;
			PSOCollectorIndex = Entry.PSOCollectorIndex;
		}
		// Factories can query other passes; never invoke them under the registry lock.
		auto Processor = Create ? Create(PSOCollectorIndex) : nullptr;
		if (Processor && Processor->GetPass() != Pass) return {};
		if (Processor) Processor->SetDrawListContext(Context);
		return Processor;
	}
	auto FPassProcessorManager::GetMeshPassProcessor(EShadingPath ShadingPath, EMeshPipelinePass Pass) -> const FMeshPassProcessor&
	{
		requiref(IsValidMeshPass(ShadingPath, Pass), "Invalid mesh pass processor index.");
		auto& Registry = PassProcessorRegistry();
		PassProcessorCreateFunction Create = nullptr;
		int32 PSOCollectorIndex = -1;
		{
			std::lock_guard Lock(Registry.Mutex);
			const auto& Entry = Registry.Entries[static_cast<size_t>(ShadingPath)][static_cast<size_t>(Pass)];
			if (Entry.Prototype) return *Entry.Prototype;
			Create = Entry.Create;
			PSOCollectorIndex = Entry.PSOCollectorIndex;
		}
		requiref(Create != nullptr, "Unregistered mesh pass processor.");
		auto Prototype = Create(PSOCollectorIndex);
		requiref(Prototype && Prototype->GetPass() == Pass, "Mesh pass factory returned an incompatible processor.");
		std::lock_guard Lock(Registry.Mutex);
		auto& Entry = Registry.Entries[static_cast<size_t>(ShadingPath)][static_cast<size_t>(Pass)];
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
	{
		const bool bSkyPass = GetPass() == EMeshPipelinePass::Sky || GetPass() == EMeshPipelinePass::HybridSky;
		if (GetPass() != EMeshPipelinePass::HitProxy && Material.bIsSky != bSkyPass) return false;
		const auto Purpose = GetPass() == EMeshPipelinePass::ShadowDepth ? EMeshCollectionPurpose::Shadow : EMeshCollectionPurpose::Receiver;
		return Participation.IsRelevant(Purpose) && IsMaterialRelevant(Material, bSupportsGBuffer);
	}
	auto FMeshPassProcessor::SupportsVertexFactory(const FMeshVertexFactoryImplementation& Factory) const -> bool
	{
		// Receivers are collected through Forward before GBuffer routing; shadows
		// have their own admission and must not reject otherwise valid receivers.
		return Factory.GetShaderType(GetVertexFactoryPass())
			&& (GetPass() == EMeshPipelinePass::ShadowDepth || Factory.GetShaderType(MaterialMeshPassForward));
	}
	auto FMeshPassProcessor::PreparePSOResources(const FMaterialPSOPrecacheParams& Params,
		const FRenderResourceGeneration& Generation, bool bRequired) -> std::expected<EPSOCollectorPreparation, std::string>
	{
		CheckRenderingThread();
		const auto* Type = Params.VertexFactoryData.VertexFactoryType;
		const auto Factory = Type ? FindMeshVertexFactory(Type->GetStableKey()) : nullptr;
		if (!Factory || &Factory->GetType() != Type || !IsEnabled(Params.PreparationConfig)
			|| !SupportsVertexFactory(*Factory) || !IsMeshRelevant(Params.Material, Params.PrecachePSOParams.Participation,
				Factory->GetShaderType(MaterialMeshPassGBuffer) != nullptr)) return EPSOCollectorPreparation::Skip;
		const auto Variants = GetPSOPrecacheVariants(Params.PreparationConfig, Params.PrecachePSOParams);
		if (Variants.empty() || (bRequired && std::ranges::none_of(Variants, [](const auto& V) { return V.bRequired; })))
			return EPSOCollectorPreparation::Skip;
		if (!PSOResources) PSOResources = std::make_unique<FMeshPSOPrecacheResources>();
		const auto Ready = PSOResources->Prepare(*this, Params, Generation);
		if (!Ready) return std::unexpected(Ready.error());
		return *Ready ? EPSOCollectorPreparation::Ready : EPSOCollectorPreparation::Pending;
	}
	auto FMeshPassProcessor::CollectPSOInitializers(const FSceneRenderPreparationConfig& Config,
		const FMaterialProgram& Program, const FMaterialStaticProperties& Properties, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
		const FPSOPrecacheParams& Params, FPSOPrecacheDataArray& Out) -> void
	{
		CheckRenderingThread();
		const auto& Map = Program.GetShaderMap();
		const FMaterialPlanningPassIdentity Identity{
			.ShaderMap = {.RenderLayout = Map.Layout.Identity, .ProgramIdentity = Map.ShaderMapId,
				.BlendMode = Properties.BlendMode, .ShadingModel = Properties.ShadingModel,
				.OpacityMaskThreshold = Properties.OpacityMaskThreshold},
			.bTwoSided = Properties.bTwoSided, .bIsSky = Properties.bIsSky, .DepthWritePolicy = Properties.DepthWritePolicy};
		const auto* Type = VertexFactoryData.VertexFactoryType;
		const auto Factory = Type ? FindMeshVertexFactory(Type->GetStableKey()) : nullptr;
		if (!Factory || &Factory->GetType() != Type || !IsEnabled(Config) || !SupportsVertexFactory(*Factory)
			|| !IsMeshRelevant(Identity, Params.Participation, Factory->GetShaderType(MaterialMeshPassGBuffer) != nullptr)) return;
		for (const auto& Variant : GetPSOPrecacheVariants(Config, Params))
		{
			auto PipelineState = MakeMeshPipelineState(Identity, Variant.Raster);
			PipelineState.bGBufferModelData = GetPass() == EMeshPipelinePass::GBuffer
											  && (Variant.bGBufferModelData || Properties.ShadingModel == EMaterialShadingModel::ClearCoat);
			if (GetPass() == EMeshPipelinePass::GBuffer && Properties.ShadingModel == EMaterialShadingModel::ClearCoat && !Variant.bGBufferModelData) continue;
			const FMeshPassProcessorBatch Batch{Identity, *Factory, PipelineState, Params.Topology, Params.Participation, true, Variant.Raster.bReversedZ};
			FGraphicsPipelineStateInitializer Initializer;
			if (!BuildPSOInitializer(Batch, PSOResources->GetShaders(), PSOResources->GetDeclaration(), Initializer))
			{ requiref(false, "Prepared mesh resources do not match the append inputs."); }
			const bool bRequired = Variant.bRequired && (GetPass() != EMeshPipelinePass::GBuffer || Properties.ShadingModel == EMaterialShadingModel::ClearCoat || !Variant.bGBufferModelData);
			Out.emplace_back(Initializer, bRequired);
			Out.back().PSOCollectorIndex = PSOCollectorIndex;
			Out.back().VertexFactoryType = VertexFactoryData.VertexFactoryType;
		}
	}

}

namespace Durin
{
	using namespace RendererPrivate;
	auto FResolvedStaticMeshView::GetMeshDrawCommandPass(FMeshPassKey Pass) -> FParallelMeshDrawCommandPass&
	{
		auto& Entry = MeshDrawCommandPasses[Pass];
		if (!Entry) Entry = std::make_shared<FParallelMeshDrawCommandPass>(Pass);
		return *Entry;
	}
	auto FResolvedStaticMeshView::GetMeshDrawCommandPass(FMeshPassKey Pass) const -> const FParallelMeshDrawCommandPass&
	{
		const auto Found = MeshDrawCommandPasses.find(Pass);
		static const FParallelMeshDrawCommandPass Empty;
		return Found == MeshDrawCommandPasses.end() ? Empty : *Found->second;
	}
	auto FResolvedStaticMeshView::GetVisibleMeshDrawCommand(const FPreparedStaticMeshDraw& Draw, RendererPrivate::FMeshPassKey Pass) const
		-> const RendererPrivate::FVisibleMeshDrawCommand*
	{
		if (Draw.ResolvedIndex >= Draws.size()) return nullptr;
		return GetMeshDrawCommandPass(Pass).GetVisibleForSource(Draw.ResolvedIndex);
	}

}

namespace Durin
{
	auto FPrimitiveSceneInfo::BuildCachedMeshDrawCommands(FScene& Scene, std::span<const FStaticMeshCommandBuildRequest> Requests,
		FMeshDrawCommandBindingCache& Bindings) -> void
	{
		check(IsInRenderingThread());
		require(Scene.CachedDrawLists);
		struct FPass
		{
			FPass(FCachedMeshDrawCommandStorage& Storage, FMeshPassKey Key, FMeshDrawCommandBindingCache& Bindings)
				: Context(Storage, Key, Bindings), Processor(FPassProcessorManager::CreateMeshPassProcessor(EShadingPath::Deferred, Key.Pass, &Context)) {}
			FCachedPassMeshDrawListContext Context;
			std::unique_ptr<FMeshPassProcessor> Processor;
		};
		std::map<FMeshPassKey, FPass> Passes;
		for (const auto& Request : Requests)
		{
			const auto& Input = Request.Input;
			const auto* Owner = Input.Primitive.SceneInfo.Resolve();
			if (!Owner || Owner->Scene != &Scene) continue;
			const auto Factory = FindMeshVertexFactory(Input.Draw.Command->PipelineKey.FactoryKey);
			if (!Factory) continue;
			const auto Key = Input.Pipeline->GetPassKey();
			auto& Pass = Passes.try_emplace(Key, *Scene.CachedDrawLists, Key, Bindings).first->second;
			const auto Built = Pass.Processor->BuildMeshDrawCommands(MakeMeshPassProcessorBatch(Input, *Factory), Input);
			if (Built) Request.Result = Pass.Context.GetCommandInfoAndReset();
			else DURIN_ERROR("Static mesh command cache build failed: pass={}, error={}", static_cast<uint8>(Key.Pass), Built.Error);
		}
	}
	auto FPrimitiveSceneInfo::RemoveCachedMeshDrawCommands() -> void { if (DrawLists) DrawLists->RemovePrimitive(*this); }
	auto FPrimitiveSceneInfo::GetPrimitiveUniformBuffer(uint64 Batch) -> FUniformBuffer&
	{
		auto& Buffer = PrimitiveUniformBuffers[Batch];
		if (!Buffer) Buffer = FUniformBuffer::Create();
		return *Buffer;
	}
	auto FPrimitiveSceneInfo::RetainPrimitiveUniformBuffers(std::span<const uint64> Batches) -> void
	{
		CheckRenderingThread();
		const std::unordered_set<uint64> Live(Batches.begin(), Batches.end());
		if (std::ranges::any_of(PrimitiveUniformBuffers, [&](const auto& Entry) { return !Live.contains(Entry.first); })) RemoveCachedMeshDrawCommands();
		std::erase_if(PrimitiveUniformBuffers, [&](const auto& Entry) { return !Live.contains(Entry.first); });
	}
}
