#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialCompileTypes.h"
namespace Durin
{
	FMaterialShaderMap::FMaterialShaderMap() = default;
	FMaterialShaderMap::~FMaterialShaderMap() = default;
	FMaterialShaderMap::FMaterialShaderMap(const FMaterialShaderMap& Other)
		: ActiveParameters(Other.ActiveParameters), Layout(Other.Layout), Requirements(Other.Requirements),
		  Dependencies(Other.Dependencies), CompiledShaders(Other.CompiledShaders), ShaderMapId(Other.ShaderMapId),
          CompilerIdentity(Other.CompilerIdentity), Target(Other.Target), PassContractVersion(Other.PassContractVersion),
          RequiredStages(Other.RequiredStages), CollectionLayouts(Other.CollectionLayouts), DebugData(Other.DebugData),
          CompileTime(Other.CompileTime) {}
	auto FMaterialCompilerResult::GetShaderMap() const -> const FMaterialShaderMap&
	{ static const FMaterialShaderMap Empty; return ShaderMap ? *ShaderMap : Empty; }
	auto FMaterialCompilerResult::GetDebugData() const -> const FMaterialProgramDebugData&
	{ static const FMaterialProgramDebugData Empty; const auto& Debug = GetShaderMap().DebugData; return Debug ? *Debug : Empty; }
	auto FMaterialShaderMap::GetShaderMapId() const -> FMaterialProgramIdentity
	{
		return ShaderMapId;
	}
	auto FMaterialShaderMap::SetCollectionLayouts(std::span<const FMaterialParameterCollectionLayout> Collections) -> void
	{
		CollectionLayouts.assign(Collections.begin(), Collections.end());
		for (auto& Schema : CollectionLayouts) {
			Schema.AssetPath.clear(); Schema.AssetPath.shrink_to_fit();
			Schema.DefaultPayload.clear(); Schema.DefaultPayload.shrink_to_fit();
		}
	}

	namespace
	{
		std::mutex CacheMutex;
		std::unordered_map<FMaterialProgramIdentity, std::shared_ptr<const FMaterialShaderMap>> Programs;
		std::deque<FMaterialProgramIdentity> Order;
		uint64 Bytes = 0;
	}
	namespace
	{
		auto DebugBytes(const std::shared_ptr<const FMaterialProgramDebugData>& Debug) -> uint64
		{
			if (!Debug) return 0;
			uint64 Bytes = sizeof(*Debug) + Debug->GeneratedSource.capacity() + Debug->IR.Nodes.capacity() * sizeof(MIR::FNode);
			for (const auto& Node : Debug->IR.Nodes) Bytes += Node.Inputs.capacity() * sizeof(uint32);
			return Bytes;
		}
		auto DiagnosticBytes(const std::vector<FMaterialProgramDiagnostic>& Diagnostics) -> uint64
		{
			uint64 Bytes = Diagnostics.capacity() * sizeof(FMaterialProgramDiagnostic);
			for (const auto& Diagnostic : Diagnostics) Bytes += Diagnostic.FunctionAssetPath.capacity()
				+ Diagnostic.CallPath.capacity() * sizeof(FGuid) + Diagnostic.Message.capacity();
			return Bytes;
		}
		auto CollectionBytes(std::span<const FMaterialParameterCollectionLayout> Collections) -> uint64
		{
			uint64 Bytes = 0;
			for (const auto& Collection : Collections) Bytes += sizeof(Collection) + Collection.AssetPath.capacity()
				+ Collection.DefaultPayload.capacity() + Collection.UniformLayout.Fields.capacity() * sizeof(FMaterialRenderField);
			return Bytes;
		}
	}

	auto FMaterialShaderMap::GetRetainedBytes() const -> uint64
	{
		uint64 Size = sizeof(*this) + CompilerIdentity.capacity() + Target.capacity() + DebugBytes(DebugData)
			+ CollectionBytes(CollectionLayouts)
			+ ActiveParameters.capacity() * sizeof(FMaterialCompilerParameterDeclaration)
			+ Layout.Fields.capacity() * sizeof(FMaterialRenderField) + CompiledShaders.capacity() * sizeof(FShaderCode);
		for (const auto& Dependency : Dependencies) Size += sizeof(Dependency) + Dependency.VirtualPath.capacity();
		for (const auto& Shader : CompiledShaders) {
			Size += Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity() + Shader.Code.GetRetainedCapacityBytes();
			for (const auto& Binding : Shader.Reflection.ResourceBindings) Size += sizeof(Binding) + Binding.Name.capacity();
			Size += Shader.Reflection.PushConstantRanges.capacity() * sizeof(FPushConstantRange) + Shader.PlatformData.GetRetainedCapacityBytes();
		}
		return Size;
	}
	auto MaterialBuildPrivate::GetMaterialProgramRetainedBytes(const FMaterialCompilerResult& Result) -> uint64
	{
		// Charge shared backing storage in full for each independently retained result.
		return sizeof(Result) + (Result.ShaderMap ? Result.ShaderMap->GetRetainedBytes() : 0)
			+ Result.StaticBools.capacity() * sizeof(FMaterialCompilerEnvironment::FStaticBoolValue)
			+ CollectionBytes(Result.ActiveCollections) + DiagnosticBytes(Result.Diagnostics);
	}
	auto FMaterialShaderMap::FindId(FMaterialProgramIdentity Identity) -> std::shared_ptr<const FMaterialShaderMap>
	{ std::lock_guard Lock(CacheMutex); const auto Found = Programs.find(Identity); return Found == Programs.end() ? nullptr : Found->second; }

	auto MaterialBuildPrivate::BindMaterialProgramCollections(std::shared_ptr<const FMaterialCompilerResult> Program,
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
	auto FMaterialShaderMap::Register(std::shared_ptr<const FMaterialShaderMap> ShaderMap, bool Replace)
		-> std::shared_ptr<const FMaterialShaderMap>
	{
		if (!ShaderMap || ShaderMap->CompiledShaders.empty()) return ShaderMap;
		const auto Size = ShaderMap->GetRetainedBytes(); if (Size > 8ull * 1024 * 1024) return ShaderMap;
		std::lock_guard Lock(CacheMutex);
		const auto Found = Programs.find(ShaderMap->ShaderMapId);
		if (Found != Programs.end()) {
			if (!Replace) return Found->second;
			Bytes -= Found->second->GetRetainedBytes(); Programs.erase(Found); std::erase(Order, ShaderMap->ShaderMapId);
		}
		while (!Order.empty() && (Programs.size() >= 128 || Bytes + Size > 256ull * 1024 * 1024)) {
			const auto Old = Order.front(); Order.pop_front(); Bytes -= Programs.at(Old)->GetRetainedBytes(); Programs.erase(Old);
		}
		Bytes += Size; Programs.emplace(ShaderMap->ShaderMapId, ShaderMap); Order.push_back(ShaderMap->ShaderMapId); return ShaderMap;
	}

	auto FMaterialShaderMap::ClearCache() -> void { std::lock_guard Lock(CacheMutex); Programs.clear(); Order.clear(); Bytes = 0; }
	auto FMaterialShaderMap::GetCacheStats() -> std::pair<uint32, uint64> { std::lock_guard Lock(CacheMutex); return {uint32(Programs.size()), Bytes}; }

}
