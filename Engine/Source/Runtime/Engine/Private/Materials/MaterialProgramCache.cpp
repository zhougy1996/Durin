#include "MaterialProgramCache.h"
namespace Durin::MaterialBuildPrivate
{
	namespace
	{
		std::mutex Mutex;
		struct FProgramEntry { std::shared_ptr<const FMaterialCompilerResult> Program; uint64 Bytes = 0; };
		std::unordered_map<FMaterialProgramIdentity, FProgramEntry> Programs;
		std::deque<FMaterialProgramIdentity> Order;
		uint64 Bytes = 0;
	}
	auto EstimateProgramBytes(const FMaterialCompilerResult& Result) -> uint64
	{
		const auto& Artifact = Result.GetArtifact();
		const auto& DebugData = Result.GetDebugData();
		// Charge the full transitive capacity even when another program also shares it.
		uint64 Size = sizeof(Result) + (Result.Artifact ? sizeof(FMaterialProgramArtifact) : 0)
			+ (Result.DebugData ? sizeof(FMaterialProgramDebugData) : 0)
			+ Result.StaticBools.capacity() * sizeof(FMaterialCompilerEnvironment::FStaticBoolValue)
			+ DebugData.GeneratedSource.capacity() + Result.CompilerIdentity.capacity() + Result.Target.capacity()
			+ DebugData.IR.Nodes.capacity() * sizeof(MIR::FNode) + Artifact.ActiveParameters.capacity() * sizeof(FMaterialCompilerParameterDeclaration)
			+ Artifact.Layout.Fields.capacity() * sizeof(FMaterialRenderField) + Artifact.CompiledShaders.capacity() * sizeof(FShaderCode);
		for (const auto& Node : DebugData.IR.Nodes) Size += Node.Inputs.capacity() * sizeof(uint32);
		for (const auto& Dependency : Artifact.Dependencies) Size += sizeof(Dependency) + Dependency.VirtualPath.capacity();
		for (const auto& Collection : Result.ActiveCollections) Size += sizeof(Collection) + Collection.AssetPath.capacity() + Collection.DefaultPayload.capacity() + Collection.UniformLayout.Fields.capacity() * sizeof(FMaterialRenderField);
		for (const auto& Shader : Artifact.CompiledShaders)
		{
			Size += Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity() + Shader.Code.GetRetainedCapacityBytes();
			for (const auto& Binding : Shader.Reflection.ResourceBindings) Size += sizeof(Binding) + Binding.Name.capacity();
			Size += Shader.Reflection.PushConstantRanges.capacity() * sizeof(FPushConstantRange) + Shader.PlatformData.GetRetainedCapacityBytes();
		}
		return Size;
	}
	auto FindProgram(FMaterialProgramIdentity Identity) -> std::shared_ptr<const FMaterialCompilerResult>
	{ std::lock_guard Lock(Mutex); const auto Found = Programs.find(Identity); return Found == Programs.end() ? nullptr : Found->second.Program; }
	auto BindProgramCollections(std::shared_ptr<const FMaterialCompilerResult> Program,
		std::span<const FMaterialParameterCollectionLayout> Collections, const FMaterialCompilerEnvironment* Environment) -> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (!Program) return {};
		bool Changed = Environment && (Program->Quality != Environment->Quality || Program->FeatureLevel != Environment->FeatureLevel || Program->StaticBools != Environment->StaticBools);
		for (const auto& Collection : Program->ActiveCollections)
		{
			const auto Found = std::ranges::find(Collections, Collection.CollectionId, &FMaterialParameterCollectionLayout::CollectionId);
			if (Found == Collections.end() || !Collection.HasCompatibleSchema(*Found)) return {};
			Changed |= Collection.DefaultPayload != Found->DefaultPayload || (Environment && Collection.AssetPath != Found->AssetPath);
		}
		if (!Changed) return Program;
		auto Result = std::make_shared<FMaterialCompilerResult>(*Program);
		if (Environment) { Result->Quality = Environment->Quality; Result->FeatureLevel = Environment->FeatureLevel; Result->StaticBools = Environment->StaticBools; }
		for (auto& Collection : Result->ActiveCollections) Collection = *std::ranges::find(Collections, Collection.CollectionId, &FMaterialParameterCollectionLayout::CollectionId);
		return Result;
	}
	auto ConfigureProgram(std::shared_ptr<const FMaterialCompilerResult> Program, const MIR::FCompilerInput& Input) -> std::shared_ptr<const FMaterialCompilerResult>
	{ return BindProgramCollections(std::move(Program), Input.Collections, &Input.Environment); }

	auto RetainProgram(std::shared_ptr<const FMaterialCompilerResult> Program, bool Replace) -> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (!Program || !*Program) return Program;
		const auto Size = EstimateProgramBytes(*Program); if (Size > 8ull * 1024 * 1024) return Program;
		std::lock_guard Lock(Mutex);
		const auto Found = Programs.find(Program->Identity);
		if (Found != Programs.end())
		{
			if (!Replace) return Found->second.Program;
			Bytes -= Found->second.Bytes; Programs.erase(Found); std::erase(Order, Program->Identity);
		}
		while (!Order.empty() && (Programs.size() >= 128 || Bytes + Size > 256ull * 1024 * 1024))
		{ auto Old = Order.front(); Order.pop_front(); Bytes -= Programs.at(Old).Bytes; Programs.erase(Old); }
		Bytes += Size; Programs.emplace(Program->Identity, FProgramEntry{Program, Size}); Order.push_back(Program->Identity); return Program;
	}
	auto ClearPrograms() -> void { std::lock_guard Lock(Mutex); Programs.clear(); Order.clear(); Bytes = 0; }
	auto ProgramCacheStats() -> std::pair<uint32, uint64> { std::lock_guard Lock(Mutex); return {uint32(Programs.size()), Bytes}; }
}
