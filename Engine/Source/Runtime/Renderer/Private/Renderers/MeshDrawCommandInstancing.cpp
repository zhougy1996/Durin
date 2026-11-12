#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshVertexFactory.h"
#include "RHICommandList.h"

namespace Durin::RendererPrivate
{
	namespace
	{
		auto Combine(size_t& Hash, uint64 Value) -> void
		{ Hash ^= std::hash<uint64>{}(Value) + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2); }
		auto BindingHash(const FMeshDrawShaderBindings& Bindings) -> size_t
		{
			size_t Hash = reinterpret_cast<uintptr_t>(Bindings.Shader);
			for (const auto& Binding : Bindings.Parameters)
			{
				Combine(Hash, reinterpret_cast<uintptr_t>(Binding.Resource));
				for (uint64 Value : {uint64(Binding.SetIndex), uint64(Binding.BindingIndex), uint64(Binding.ArrayElement),
					uint64(Binding.Type), uint64(Binding.Offset), uint64(Binding.Size), uint64(Binding.ResourceIndex)}) Combine(Hash, Value);
			}
			return Hash;
		}
		auto GeometryWithoutInstanceOffset(const FMeshDrawCommand& Command)
		{
			auto Args = Command.DrawParameters;
			std::visit([](auto& Draw) { Draw.FirstInstance = 0; }, Args);
			return Args;
		}
	}

	auto FMeshDrawCommand::MatchesForDynamicInstancing(const FMeshDrawCommand& Other) const -> bool
	{
		if (!bFinalized || !Other.bFinalized || !Pipeline || !Other.Pipeline || IndirectArgs || Other.IndirectArgs
			|| !Pipeline->VertexDeclaration || !Other.Pipeline->VertexDeclaration) return false;
		return Pipeline->Pass == Other.Pipeline->Pass && Pipeline->Description == Other.Pipeline->Description
			&& Pipeline->FactoryKey == Other.Pipeline->FactoryKey && Pipeline->LayoutKey == Other.Pipeline->LayoutKey
			&& Pipeline->FragmentShader == Other.Pipeline->FragmentShader
			&& Pipeline->VertexDeclaration->GetElements() == Other.Pipeline->VertexDeclaration->GetElements()
			&& bool(Pipeline->ShaderMap) == bool(Other.Pipeline->ShaderMap)
			&& (!Pipeline->ShaderMap || Pipeline->ShaderMap.GetPipelineLayout() == Other.Pipeline->ShaderMap.GetPipelineLayout())
			&& VertexStreams == Other.VertexStreams && IndexBuffer == Other.IndexBuffer && IndexBufferOffset == Other.IndexBufferOffset
			&& GeometryWithoutInstanceOffset(*this) == GeometryWithoutInstanceOffset(Other)
			&& VertexBindings == Other.VertexBindings && FragmentBindings == Other.FragmentBindings && DepthBias == Other.DepthBias;
	}

	auto FMeshDrawCommand::GetDynamicInstancingHash() const -> uint32
	{
		// A hash only narrows candidates. Full effective state, including PSO values,
		// layout and bindings, is compared before a command enters the same bucket.
		size_t Hash = BindingHash(VertexBindings); Combine(Hash, BindingHash(FragmentBindings));
		Combine(Hash, reinterpret_cast<uintptr_t>(IndexBuffer)); Combine(Hash, IndexBufferOffset);
		for (const auto& Stream : VertexStreams)
		{
			Combine(Hash, Stream.StreamIndex); Combine(Hash, reinterpret_cast<uintptr_t>(Stream.VertexBuffer));
			Combine(Hash, Stream.Offset); Combine(Hash, Stream.Stride);
		}
		Combine(Hash, DrawParameters.index());
		std::visit([&](const auto& Draw) {
			Combine(Hash, Draw.InstanceCount);
			if constexpr (std::same_as<std::decay_t<decltype(Draw)>, FRHIDrawArguments>)
			{ Combine(Hash, Draw.VertexCount); Combine(Hash, Draw.FirstVertex); }
			else { Combine(Hash, Draw.IndexCount); Combine(Hash, Draw.FirstIndex); Combine(Hash, uint32(Draw.VertexOffset)); }
		}, DrawParameters);
		for (float Bias : DepthBias) Combine(Hash, std::hash<float>{}(Bias));
		return uint32(Hash) ^ uint32(Hash >> 32);
	}

	auto FDynamicMeshDrawCommandStorage::PublishVisible(FVisibleMeshDrawCommand Visible,
		std::shared_ptr<const FMeshPassBindings> Pass) -> FMeshDrawCommandHandle
	{
		require(Pass && Visible.Command && Visible.Command->Pipeline);
		const auto Key = Visible.Command->Pipeline->GetPassKey();
		auto& List = Data->Visible[Key];
		Visible.Pass = Pass.get(); List.push_back(Visible);
		Data->RetainedPasses.push_back(std::move(Pass));
		return {Key, List.size() - 1, Data->Id};
	}

	auto CollectMeshDrawCommands(const FPreparedStaticMeshView& Prepared,
		const FResolvedStaticMeshView& Resolved, FMeshPassKey Pass, bool bAllowDynamicInstancing) -> FMeshCommandOneFrameArray
	{
		FMeshCommandOneFrameArray Result(Resolved.Draws.size());
		for (uint32 Index = 0; Index < Prepared.GetNumSections(); ++Index)
		{
			const auto& Draw = Prepared.GetDraw(Index);
			const auto* Primitive = Prepared.GetPrimitive(Draw);
			if (!Primitive || !Resolved.IsReady(Draw) || Draw.ResolvedIndex >= Result.size()) continue;
			const auto Command = Resolved.GetMeshDrawCommand(Draw, Pass);
			if (!Command) continue;
			auto& Visible = Result[Draw.ResolvedIndex];
			if (const auto* Existing = Resolved.GetVisibleMeshDrawCommand(Draw, Pass, bAllowDynamicInstancing)) Visible = *Existing;
			else
			{
				auto Bindings = std::make_shared<FMeshPassBindings>(FMeshPassBindings{Command->VertexPassBindings, Command->FragmentPassBindings});
				const auto Handle = Resolved.DrawCommandStorage->PublishVisible({.Command = Command.get(),
					.DepthBias = Command->DepthBias, .PrimitiveIdBufferOffset = Command->InstanceIdOffset}, Bindings);
				Visible = *Resolved.DrawCommandStorage->GetVisible(Handle);
			}
			if (Draw.PrimitiveIndex < Resolved.GPUScenePrimitives.size()) Visible.SceneData = Resolved.GPUScenePrimitives[Draw.PrimitiveIndex];
			Visible.bSupportsInstanceCulling = Index < Prepared.Opaque.size() + Prepared.Masked.size() && Primitive->VertexDomain == EVertexDeformationDomain::Local
				&& Draw.Command && Draw.Command->Geometry.bIndexed && Draw.Command->Geometry.InstanceCount == 1
				&& (Pass.Pass != EMeshPipelinePass::GBuffer || Draw.Command->bSupportsGBuffer)
				&& Primitive->WorldBounds.bIsValid && Math::IsFinite(Primitive->WorldBounds.Min) && Math::IsFinite(Primitive->WorldBounds.Max)
				&& Primitive->CollectedBinding && std::ranges::none_of(Primitive->CollectedBinding->DeclarationElements,
					[](const auto& Element) { return Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance; });
		}
		return Result;
	}

	auto SetupMeshDrawCommandDynamicInstancing(FRHICommandListImmediate& Commands,
		const FPreparedStaticMeshView& Prepared, FResolvedStaticMeshView& Resolved) -> bool
	{
		struct FCandidate
		{
			const FPreparedStaticMeshDraw* Draw;
			const FMeshDrawCommand* Command;
			FMeshPassBindings Bindings;
			std::array<float, 3> Bias;
			uint32 InstanceId;
			int32 StateBucketId;
		};
		struct FRun { FMeshPassKey Pass; std::vector<FCandidate> Candidates; };
		std::vector<FRun> Runs;
		std::vector<uint32> Ids;
		for (const auto Pass : {EMeshPipelinePass::Forward, EMeshPipelinePass::HybridRetainedForward, EMeshPipelinePass::ShadowDepth, EMeshPipelinePass::GBuffer})
		for (const auto* Bucket : {&Prepared.Opaque, &Prepared.Masked})
		{
			std::unordered_map<size_t, std::vector<FCandidate>> Buckets;
			int32 NextBucket = 0;
			bool bContinueRun = false;
			for (const auto& Draw : *Bucket)
			{
				const auto* Primitive = Prepared.GetPrimitive(Draw);
				const auto Command = Resolved.GetMeshDrawCommand(Draw, {Pass});
				if (!Primitive || !Resolved.IsReady(Draw) || !Command || Command->IndirectArgs
					|| Primitive->VertexDomain != EVertexDeformationDomain::Local || Draw.Command->Geometry.InstanceCount != 1
					|| Draw.PrimitiveIndex >= Resolved.GPUScenePrimitives.size() || !Resolved.GPUScenePrimitives[Draw.PrimitiveIndex].Buffers
					|| std::ranges::any_of(Primitive->CollectedBinding->DeclarationElements,
						[](const auto& Element) { return Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance; }))
				{ bContinueRun = false; continue; }
				FMeshPassBindings Bindings{Command->VertexPassBindings, Command->FragmentPassBindings};
				auto Bias = Command->DepthBias;
				if (const auto* Visible = Resolved.GetVisibleMeshDrawCommand(Draw, {Pass}))
				{ Bindings = *Visible->Pass; Bias = Visible->DepthBias; }
				FCandidate Candidate{&Draw, Command.get(), std::move(Bindings), Bias,
					Resolved.GPUScenePrimitives[Draw.PrimitiveIndex].InstanceId, INDEX_NONE};
				size_t Hash = Command->GetDynamicInstancingHash();
				Combine(Hash, BindingHash(Candidate.Bindings.Vertex)); Combine(Hash, BindingHash(Candidate.Bindings.Fragment));
				for (float Value : Bias) Combine(Hash, std::hash<float>{}(Value));
				auto& States = Buckets[Hash];
				const auto Found = std::ranges::find_if(States, [&](const auto& Other) {
					return Command->MatchesForDynamicInstancing(*Other.Command) && Candidate.Bias == Other.Bias
						&& Candidate.Bindings.Vertex == Other.Bindings.Vertex && Candidate.Bindings.Fragment == Other.Bindings.Fragment;
				});
				if (Found == States.end()) { Candidate.StateBucketId = NextBucket++; States.push_back(Candidate); }
				else Candidate.StateBucketId = Found->StateBucketId;
				// Keep the prepared order. Never pull a command across an intervening
				// incompatible draw, and never visit sorted translucent buckets.
				if (!bContinueRun || Runs.back().Candidates.back().StateBucketId != Candidate.StateBucketId)
					Runs.push_back({{Pass}, {}});
				Runs.back().Candidates.push_back(std::move(Candidate)); bContinueRun = true;
			}
		}
		for (const auto& Run : Runs) if (Run.Candidates.size() > 1)
			for (const auto& Candidate : Run.Candidates) Ids.push_back(Candidate.InstanceId);
		if (Ids.empty()) return true;
		if (Ids.size() > UINT32_MAX / sizeof(uint32)) return false;
		Resolved.DynamicInstancingInstanceIds = FGPUSceneDynamicContext::UploadInstanceIds(Commands, Ids);
		if (!Resolved.DynamicInstancingInstanceIds) return false;
		uint32 Offset = 0;
		for (const auto& Run : Runs)
		{
			if (Run.Candidates.size() < 2) continue;
			const uint32 GroupIndex = static_cast<uint32>(Resolved.DynamicInstancingGroups.size());
			auto& Group = Resolved.DynamicInstancingGroups.emplace_back(); Group.Pass = Run.Pass;
			auto Bindings = std::make_shared<FMeshPassBindings>(Run.Candidates.front().Bindings);
			for (size_t Index = 0; Index < Run.Candidates.size(); ++Index)
			{
				const auto& Candidate = Run.Candidates[Index];
				Group.Draws.push_back(Candidate.Draw->ResolvedIndex);
				const auto Handle = Resolved.DrawCommandStorage->PublishVisible({.Command = Candidate.Command,
					.DepthBias = Candidate.Bias, .PrimitiveIdBufferOffset = Offset,
					.NumInstances = static_cast<uint32>(Run.Candidates.size()), .bSkipDraw = Index != 0,
					.StateBucketId = Candidate.StateBucketId, .InstanceBuffer = Resolved.DynamicInstancingInstanceIds}, Bindings);
				Resolved.Draws[Candidate.Draw->ResolvedIndex].DynamicInstancingCommands[Run.Pass] = {GroupIndex, Handle};
			}
			Offset += static_cast<uint32>(Run.Candidates.size());
		}
		return true;
	}
}
