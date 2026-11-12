#include "Renderers/GPUSceneRendering.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "GPUScene.h"
#include "Renderers/MeshVertexFactory.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"

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

	auto FGPUSceneDynamicContext::Prepare(FRHICommandListImmediate& Commands,
		const FPreparedStaticMeshView& Prepared, FGPUScene* Scene) -> std::vector<FGPUScenePrimitiveData>
	{
		std::vector<FGPUScenePrimitiveData> Result(Prepared.Primitives.size());
		auto Persistent = std::make_shared<FGPUSceneBufferSet>();
		auto Dynamic = std::make_shared<FGPUSceneBufferSet>();
		if (Scene) Persistent->SceneData = Scene->GetResourceBuffers();
		std::array<std::vector<uint32>, 2> Ids;
		std::vector<FPrimitiveSceneShaderData> Primitives;
		std::vector<FInstanceSceneShaderData> Instances;
		std::vector<FPrimitiveCustomData> Payload;
		std::vector<uint32> Counts(Result.size(), 1);
		std::vector<bool> Supported(Result.size());
		for (uint32 Index = 0; Index < Prepared.GetNumSections(); ++Index)
		{
			const auto& Draw = Prepared.GetDraw(Index);
			if (Draw.Command)
			{
				Counts[Draw.PrimitiveIndex] = std::max(Counts[Draw.PrimitiveIndex], Draw.Command->Geometry.InstanceCount);
				const auto Factory = RendererPrivate::FindMeshVertexFactory(Draw.Command->PipelineKey.FactoryKey);
				if (Factory && Factory->SupportsGPUScene()) Supported[Draw.PrimitiveIndex] = true;
			}
		}
		for (uint32 Index = 0; Index < Result.size(); ++Index)
		{
			if (!Supported[Index]) continue;
			const auto& Primitive = Prepared.Primitives[Index];
			std::optional<uint32> Existing;
			if (Scene && Primitive.PrimitiveIdMode == EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo)
			{
				Existing = Scene->FindInstanceId(Primitive.PersistentPrimitiveIndex, Primitive.InstanceSceneDataId);
				// A broken persistent reference is an admission failure, never inferred dynamic data.
				if (!Existing || !Persistent->SceneData[0]) return {};
			}
			uint32 InstanceId;
			if (Existing) InstanceId = *Existing;
			else
			{
				InstanceId = static_cast<uint32>(Instances.size());
				const auto Parameters = FPrimitiveUniformShaderParametersBuilder().LocalToWorld(Primitive.LocalToWorld)
					.WorldBounds(Primitive.WorldBounds).CustomPrimitiveData(Primitive.CustomPrimitiveData).InstanceSceneData(InstanceId, 1).Build();
				if (!Parameters) return {};
				Primitives.emplace_back(*Parameters);
				FInstanceSceneShaderData Data;
				if (!Data.Build(InstanceId, 0, InstanceId * 8, {}, Primitive.LocalToWorld, Primitive.WorldBounds)) return {};
				Instances.push_back(Data); Payload.push_back(Primitive.CustomInstanceData);
			}
			const uint32 Group = Existing ? 0 : 1;
			if (uint64(Ids[Group].size()) + Counts[Index] > UINT32_MAX / sizeof(uint32)) return {};
			Result[Index] = {Existing ? Persistent : Dynamic, InstanceId, static_cast<uint32>(Ids[Group].size())};
			Ids[Group].insert(Ids[Group].end(), Counts[Index], InstanceId);
		}
		if (!Instances.empty())
		{
			Dynamic->SceneData = {UploadGPUSceneBuffer(Commands, "DynamicGPUScene.Primitives", 16, std::as_bytes(std::span(Primitives))),
				UploadGPUSceneBuffer(Commands, "DynamicGPUScene.Instances", 16, std::as_bytes(std::span(Instances))),
				UploadGPUSceneBuffer(Commands, "DynamicGPUScene.Payload", 16, std::as_bytes(std::span(Payload)))};
		}
		Persistent->InstanceIds = UploadGPUSceneBuffer(Commands, "GPUScene.DirectInstanceIds", 4, std::as_bytes(std::span(Ids[0])));
		Dynamic->InstanceIds = UploadGPUSceneBuffer(Commands, "DynamicGPUScene.DirectInstanceIds", 4, std::as_bytes(std::span(Ids[1])));
		return Result;
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
