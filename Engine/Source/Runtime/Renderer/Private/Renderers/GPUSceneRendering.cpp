#include "Renderers/GPUSceneRendering.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "GPUScene.h"
#include "Renderers/MeshVertexFactory.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	static auto UploadGPUSceneBuffer(FRHICommandListImmediate& Commands, std::string_view Name, uint32 Stride, FByteView Bytes) -> FBufferRHIRef
	{
		if (Bytes.empty()) return {};
		auto Buffer = GDynamicRHI->RHICreateBuffer(Commands, FRHIBufferCreateDesc::Create(std::string(Name).c_str(),
			static_cast<uint32>(Bytes.size()), Stride, EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::DestinationCopy));
		if (!Buffer) return {};
		const std::array Before{FRHIBufferTransition::Whole(Buffer, ERHIAccess::None, ERHIAccess::TransferWrite)};
		Commands.TransitionBuffers(Before); Commands.WriteBuffer(Buffer, 0, Bytes);
		const std::array After{FRHIBufferTransition::Whole(Buffer, ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead)};
		Commands.TransitionBuffers(After);
		return Buffer;
	}

	auto FGPUSceneDynamicContext::UploadInstanceIds(FRHICommandListImmediate& Commands, std::span<const uint32> Ids) -> FBufferRHIRef
	{ return UploadGPUSceneBuffer(Commands, "GPUScene.InstanceIds", 4, std::as_bytes(Ids)); }

	auto FGPUSceneBufferSet::GetShaderParameters() const -> FGPUSceneShaderParameters
	{
		auto Range = [](const FBufferRHIRef& Buffer) -> FRHIStorageBufferRange { return {Buffer, 0, Buffer ? Buffer->GetSize() : 0}; };
		return {Range(SceneData[0]), Range(SceneData[1]), Range(SceneData[2]), Range(InstanceIds)};
	}

	FGPUSceneDynamicContext::FGPUSceneDynamicContext(std::shared_ptr<FGPUScene> InGPUScene) : GPUScene(std::move(InGPUScene)) { require(GPUScene); }
	FGPUSceneDynamicContext::~FGPUSceneDynamicContext()
	{
		if (Allocations.empty()) return;
		if (IsInRenderingThread()) for (const auto& Range : Allocations) GPUScene->ReleaseDynamicPrimitiveRange(Range);
		else EnqueueRenderCommand("ReleaseDynamicGPUSceneRanges", [Scene = std::move(GPUScene), Ranges = std::move(Allocations)](FRHICommandListImmediate&) {
			for (const auto& Range : Ranges) Scene->ReleaseDynamicPrimitiveRange(Range);
		});
	}
	auto FGPUSceneDynamicContext::RegisterGraphUse(const FRDGBuilder& Graph) -> void { GPUScene->RegisterGraphUse(Graph); }
	auto FGPUSceneDynamicContext::Prepare(FRHICommandListImmediate& Commands, FRDGAllocator& Allocator,
		const FPreparedStaticMeshView& Prepared, FGPUScene* Scene) -> std::vector<FGPUScenePrimitiveData>
	{
		CheckRenderingThread();
		std::vector<FGPUScenePrimitiveData> Result(Prepared.Primitives.size());
		std::vector<uint32> Counts(Result.size(), 1);
		std::vector<bool> Supported(Result.size());
		for (uint32 Index = 0; Index < Prepared.GetNumSections(); ++Index)
		{
			const auto& Draw = Prepared.GetDraw(Index);
			if (!Draw.Command) continue;
			Counts[Draw.PrimitiveIndex] = std::max(Counts[Draw.PrimitiveIndex], Draw.Command->Geometry.InstanceCount);
			const auto Factory = RendererPrivate::FindMeshVertexFactory(Draw.Command->PipelineKey.FactoryKey);
			if (Factory && Factory->SupportsGPUScene()) Supported[Draw.PrimitiveIndex] = true;
		}
		if (std::ranges::none_of(Supported, [](bool Value) { return Value; })) return Result;
		auto Resource = Scene ? Scene->weak_from_this().lock() : CreateRenderResource<FGPUScene>();
		if (!Resource) return {};
		auto Context = std::make_shared<FGPUSceneDynamicContext>(Resource);
		auto Commit = [&] {
			if (!Resource->IsDirty()) return true;
			FRDGBuilder UploadGraph;
			auto Upload = Resource->Update(UploadGraph);
			const auto Executed = UploadGraph.Execute(Commands, &Allocator);
			return Resource->CompleteUpdate(UploadGraph, Upload) && bool(Executed);
		};
		// Persistent stable-ID lookup needs the committed membership mirror.
		if (Scene && !Commit()) return {};
		uint32 DynamicCount = 0;
		for (uint32 Index = 0; Index < Result.size(); ++Index)
			if (Supported[Index] && (!Scene || Prepared.Primitives[Index].PrimitiveIdMode != EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo)) ++DynamicCount;
		const auto Allocation = Resource->AllocateDynamicPrimitiveRange(DynamicCount);
		if (Allocation.Count) Context->Allocations.push_back(Allocation);
		auto Buffers = std::make_shared<FGPUSceneBufferSet>(); Buffers->DynamicContext = Context; Buffers->bStandaloneScene = !Scene;
		std::vector<uint32> Ids;
		uint32 DynamicIndex = 0;
		for (uint32 Index = 0; Index < Result.size(); ++Index)
		{
			if (!Supported[Index]) continue;
			const auto& Primitive = Prepared.Primitives[Index];
			uint32 InstanceId;
			if (Scene && Primitive.PrimitiveIdMode == EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo)
			{
				const auto Existing = Resource->FindInstanceId(Primitive.PersistentPrimitiveIndex, Primitive.InstanceSceneDataId);
				if (!Existing) return {};
				InstanceId = *Existing;
			}
			else
			{
				if (!Resource->WriteDynamicPrimitive(Allocation, DynamicIndex, Primitive.LocalToWorld, Primitive.WorldBounds,
					Primitive.CustomPrimitiveData, Primitive.CustomInstanceData)) return {};
				InstanceId = Allocation.InstanceOffset + DynamicIndex++;
			}
			if (uint64(Ids.size()) + Counts[Index] > UINT32_MAX / sizeof(uint32)) return {};
			Result[Index] = {Buffers, InstanceId, static_cast<uint32>(Ids.size())};
			Ids.insert(Ids.end(), Counts[Index], InstanceId);
		}
		if (!Commit()) return {};
		Buffers->SceneData = Resource->GetResourceBuffers();
		Buffers->InstanceIds = UploadInstanceIds(Commands, Ids);
		if (!Buffers->InstanceIds) return {};
		return Result;
	}
	auto RegisterGPUSceneGraphUse(const FRDGBuilder& Graph, const FResolvedStaticMeshView& View) -> void
	{
		std::unordered_set<FGPUSceneDynamicContext*> Registered;
		for (const auto& Primitive : View.GPUScenePrimitives)
			if (Primitive.Buffers && Primitive.Buffers->DynamicContext && Registered.insert(Primitive.Buffers->DynamicContext.get()).second)
				Primitive.Buffers->DynamicContext->RegisterGraphUse(Graph);
	}

	auto FGPUSceneReadParameters::GetRDGParametersMetadata() -> const FRDGParametersMetadata*
	{
		using P = FGPUSceneReadParameters;
		static const std::array Members{MakeRDGBufferAccessMetadata<P, decltype(Buffers)>("Buffers", offsetof(P, Buffers))};
		static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FGPUSceneReadParameters", Members);
		return &Metadata;
	}
	auto FGPUSceneReadParameters::Add(FRDGBuilder& Graph, const FResolvedStaticMeshView& View) -> void
	{
		auto AddBuffer = [&](const FBufferRHIRef& Resource) {
			if (!Resource) return;
			const auto Handle = Graph.RegisterExternalBuffer(Resource, std::format("GPUScene.Read.{}", reinterpret_cast<uintptr_t>(Resource.GetReference())),
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			if (std::ranges::any_of(Buffers, [&](const auto& Item) { return Item && Item->Buffer == Handle; })) return;
			const auto Free = std::ranges::find_if(Buffers, [](const auto& Item) { return !Item; });
			require(Free != Buffers.end());
			*Free = FRDGBufferAccess{Handle, 0, Resource->GetSize(), ERDGUse::Read, ERHIAccess::GraphicsShaderRead};
		};
		AddBuffer(View.DynamicInstancingInstanceIds);
		for (const auto& Primitive : View.GPUScenePrimitives)
		{
			if (!Primitive.Buffers) continue;
			const auto& Set = *Primitive.Buffers;
			const std::array Resources{Set.SceneData[0], Set.SceneData[1], Set.SceneData[2], Set.InstanceIds};
			for (const auto& Resource : Resources) AddBuffer(Resource);
		}
	}
}
