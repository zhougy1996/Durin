#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshVertexFactory.h"
#include "RHICommandList.h"
#include "InstanceUniformShaderParameters.h"

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
			Visible.bSupportsDynamicInstancing = Index < Prepared.Opaque.size() + Prepared.Masked.size()
				&& Primitive->VertexDomain == EVertexDeformationDomain::Local && Draw.Command && Draw.Command->Geometry.InstanceCount == 1
				&& Primitive->CollectedBinding && std::ranges::none_of(Primitive->CollectedBinding->DeclarationElements,
					[](const auto& Element) { return Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance; });
			Visible.bSupportsInstanceCulling = Index < Prepared.Opaque.size() + Prepared.Masked.size() && Primitive->VertexDomain == EVertexDeformationDomain::Local
				&& Draw.Command && Draw.Command->Geometry.bIndexed && Draw.Command->Geometry.InstanceCount == 1
				&& (Pass.Pass != EMeshPipelinePass::GBuffer || Draw.Command->bSupportsGBuffer)
				&& Primitive->WorldBounds.bIsValid && Math::IsFinite(Primitive->WorldBounds.Min) && Math::IsFinite(Primitive->WorldBounds.Max)
				&& Primitive->CollectedBinding && std::ranges::none_of(Primitive->CollectedBinding->DeclarationElements,
					[](const auto& Element) { return Element.InputRate == FRHIVertexElementIdentity::EInputRate::Instance; });
		}
		return Result;
	}

	auto BuildMeshDrawCommandRuns(std::span<const FVisibleMeshDrawCommand> Visible) -> std::vector<FMeshDrawCommandRun>
	{
		std::vector<FMeshDrawCommandRun> Runs;
		for (uint32 Index = 0; Index < Visible.size(); ++Index)
		{
			const auto& Current = Visible[Index];
			if ((!Current.bSupportsDynamicInstancing && !Current.bSupportsInstanceCulling)
				|| !Current.Command || !Current.Pass || !Current.SceneData.Buffers) continue;
			const auto& Instances = Current.SceneData.Buffers->SceneData[1];
			if (Instances && Current.SceneData.InstanceId >= Instances->GetSize() / sizeof(FInstanceSceneShaderData)) continue;
			bool bContinue = false;
			if (!Runs.empty() && Runs.back().back() + 1 == Index)
			{
				const auto& Previous = Visible[Runs.back().back()];
				bContinue = Current.Command->MatchesForDynamicInstancing(*Previous.Command)
					&& Current.SceneData.Buffers->SceneData == Previous.SceneData.Buffers->SceneData
					&& Current.DepthBias == Previous.DepthBias && Current.Pass->Vertex == Previous.Pass->Vertex
					&& Current.Pass->Fragment == Previous.Pass->Fragment
					&& Current.bSupportsInstanceCulling == Previous.bSupportsInstanceCulling;
			}
			if (!bContinue) Runs.emplace_back();
			Runs.back().push_back(Index);
		}
		return Runs;
	}

	auto SetupMeshDrawCommandDynamicInstancing(FRHICommandListImmediate& Commands,
		const FPreparedStaticMeshView& Prepared, FResolvedStaticMeshView& Resolved) -> bool
	{
		struct FPassRuns { FMeshPassKey Pass; FMeshCommandOneFrameArray Visible; };
		std::vector<FPassRuns> Passes;
		std::vector<uint32> Ids;
		Resolved.MeshDrawCommandRuns.clear();
		for (const auto Pass : {EMeshPipelinePass::Forward, EMeshPipelinePass::HybridRetainedForward, EMeshPipelinePass::ShadowDepth, EMeshPipelinePass::GBuffer})
		{
			auto& Entry = Passes.emplace_back(FPassRuns{{Pass}, CollectMeshDrawCommands(Prepared, Resolved, {Pass}, false)});
			auto& Runs = Resolved.MeshDrawCommandRuns[{Pass}];
			Runs = BuildMeshDrawCommandRuns(Entry.Visible);
			for (const auto& Run : Runs) if (Run.size() > 1)
				for (uint32 Index : Run) Ids.push_back(Entry.Visible[Index].SceneData.InstanceId);
		}
		if (Ids.empty()) return true;
		if (Ids.size() > UINT32_MAX / sizeof(uint32)) return false;
		Resolved.DynamicInstancingInstanceIds = FGPUSceneDynamicContext::UploadInstanceIds(Commands, Ids);
		if (!Resolved.DynamicInstancingInstanceIds) return false;
		uint32 Offset = 0;
		for (const auto& Entry : Passes)
		for (const auto& Run : Resolved.MeshDrawCommandRuns[Entry.Pass])
		{
			if (Run.size() < 2) continue;
			const uint32 GroupIndex = static_cast<uint32>(Resolved.DynamicInstancingGroups.size());
			auto& Group = Resolved.DynamicInstancingGroups.emplace_back(); Group.Pass = Entry.Pass; Group.Draws = Run;
			const auto& Leader = Entry.Visible[Run.front()];
			auto Bindings = std::make_shared<FMeshPassBindings>(*Leader.Pass);
			for (uint32 Index : Run)
			{
				auto Visible = Entry.Visible[Index];
				Visible.PrimitiveIdBufferOffset = Offset; Visible.NumInstances = static_cast<uint32>(Run.size());
				Visible.bSkipDraw = Index != Run.front(); Visible.InstanceBuffer = Resolved.DynamicInstancingInstanceIds;
				const auto Handle = Resolved.DrawCommandStorage->PublishVisible(Visible, Bindings);
				Resolved.Draws[Index].DynamicInstancingCommands[Entry.Pass] = {GroupIndex, Handle};
			}
			Offset += static_cast<uint32>(Run.size());
		}
		return true;
	}
}
