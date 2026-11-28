#include "Materials/MaterialShaderMap.h"
#include "MaterialPreparedProgram.h"
#include "Shader/Shader.h"
#include "VertexFactory.h"
namespace Durin
{
	FMaterialShaderMap::FMaterialShaderMap() = default;
	FMaterialShaderMap::~FMaterialShaderMap() = default;
	FMaterialShaderMap::FMaterialShaderMap(const FMaterialShaderMap& Other)
		: FMaterialCompilationOutput(Other), Dependencies(Other.Dependencies), CompiledShaders(Other.CompiledShaders), ShaderMapId(Other.ShaderMapId),
          CompilerIdentity(Other.CompilerIdentity), Target(Other.Target), PassContractVersion(Other.PassContractVersion),
		  RequiredEntries(Other.RequiredEntries), DebugData(Other.DebugData),
          CompileTime(Other.CompileTime) {}
	auto FMaterialProgram::GetShaderMap() const -> const FMaterialShaderMap&
	{ static const FMaterialShaderMap Empty; return ShaderMap ? *ShaderMap : Empty; }
	auto FMaterialProgram::GetDebugData() const -> const FMaterialProgramDebugData&
	{ static const FMaterialProgramDebugData Empty; const auto& Debug = GetShaderMap().DebugData; return Debug ? *Debug : Empty; }
	auto FMaterialShaderMap::FindShaderIndex(const FShaderType& Type, const FVertexFactoryType* Factory,
		uint32 PermutationId) const -> std::optional<size_t>
	{
		const auto Entry = std::ranges::find_if(Entries, [&](const FMaterialShaderEntryRequest& Candidate) {
			return Candidate.Shader.ShaderType == Type.GetName() && Candidate.Shader.PermutationId == PermutationId
				&& Candidate.Shader.Frequency == Type.GetFrequency() && Candidate.Shader.EntryPoint == Type.GetEntryPoint()
				&& (!Candidate.Mesh || (Factory && Candidate.Mesh->VertexFactoryType == Factory->GetName()));
		});
		if (Entry == Entries.end()) return {};
		const size_t Index = Entry - Entries.begin();
		if (Index >= CompiledShaders.size() || CompiledShaders[Index].Frequency != Type.GetFrequency()
			|| CompiledShaders[Index].SourceEntryPoint != Type.GetEntryPoint()) return {};
		return Index;
	}
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
		constexpr uint64 MaxMapBytes = 8ull * 1024 * 1024;
		constexpr uint64 MaxCacheBytes = 256ull * 1024 * 1024;
		// Equivalent raw requests can share a final map. Retain the most recent source remap.
		struct FRetainedEntry {
			std::shared_ptr<const FMaterialShaderMap> Map;
			uint64 MapBytes;
			FMaterialProgramIdentity RequestId;
			struct FSourceIndex { uint32 Ordinal, ExpressionIndex; };
			std::vector<FSourceIndex> Sources;
			auto Size() const -> uint64 { return MapBytes + Sources.capacity() * sizeof(FSourceIndex); }
		};
		std::unordered_map<FMaterialProgramIdentity, FRetainedEntry> Programs;
		std::deque<FMaterialProgramIdentity> Order;
		uint64 Bytes = 0;
	}
	namespace
	{
		auto DebugBytes(const std::shared_ptr<const FMaterialProgramDebugData>& Debug) -> uint64
		{
			if (!Debug) return 0;
			uint64 Bytes = sizeof(*Debug) + Debug->GeneratedSource.capacity() + Debug->IR.Nodes.capacity() * sizeof(MIR::FNode);
			for (const auto& Node : Debug->IR.Nodes)
			{
				Bytes += Node.Inputs.capacity() * sizeof(uint32);
				if (const auto* Name = std::get_if<std::string>(&Node.Payload)) Bytes += Name->capacity();
			}
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
		Size += RequiredEntries.capacity() * sizeof(std::string) + Entries.capacity() * sizeof(FMaterialShaderEntryRequest);
		for (const auto& Parameter : ActiveParameters)
			if (const auto* Name = Parameter.Id.GetName()) Size += Name->capacity();
		for (const auto& Field : Layout.Fields)
			if (const auto* Name = Field.ParameterId.GetName()) Size += Name->capacity();
		for (const auto& Name : RequiredEntries) Size += Name.capacity();
		for (const auto& Entry : Entries)
		{
			const auto& Key = Entry.Shader; const auto& Q = Entry.Requirements;
			Size += Key.ShaderType.capacity() + Key.EntryPoint.capacity() + Key.Target.capacity();
			if (Entry.Mesh) Size += Entry.Mesh->Material.ShaderType.capacity() + Entry.Mesh->Material.EntryPoint.capacity()
				+ Entry.Mesh->Material.Target.capacity() + Entry.Mesh->VertexFactoryType.capacity();
			for (const auto& Parameter : Q.ParameterIDs)
				if (const auto* Name = Parameter.GetName()) Size += Name->capacity();
			Size += (Q.AttributeIDs.capacity() + Q.CollectionIDs.capacity()) * sizeof(FGuid)
				+ Q.ParameterIDs.capacity() * sizeof(FMaterialFieldKey)
				+ Q.ResourceIndices.capacity() * sizeof(uint16) + Q.SceneTextures.capacity() * sizeof(FMaterialSceneTextureRequirement)
				+ Q.Interpolators.capacity() * sizeof(FMaterialValueSemantics) + Entry.Bindings.capacity() * sizeof(FMaterialShaderBinding);
			for (const auto& Binding : Entry.Bindings)
				Size += Binding.Name.capacity() + Binding.QualifiedName.capacity() + Binding.OwnerName.capacity();
		}
		for (const auto& Dependency : Dependencies) Size += sizeof(Dependency) + Dependency.VirtualPath.capacity();
		for (const auto& Shader : CompiledShaders) {
			Size += Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity() + Shader.Code.GetRetainedCapacityBytes();
			for (const auto& Binding : Shader.Reflection.ResourceBindings) {
				Size += sizeof(Binding) + Binding.Name.capacity() + Binding.ConstantMembers.capacity() * sizeof(FShaderConstantMember);
				for (const auto& M : Binding.ConstantMembers) Size += M.Name.capacity();
			}
			Size += Shader.Reflection.PushConstantRanges.capacity() * sizeof(FPushConstantRange) + Shader.PlatformData.GetRetainedCapacityBytes();
		}
		return Size;
	}
	auto FMaterialShaderMap::FindId(FMaterialProgramIdentity Identity) -> std::shared_ptr<const FMaterialShaderMap>
	{ std::lock_guard Lock(CacheMutex); const auto Found = Programs.find(Identity); return Found == Programs.end() ? nullptr : Found->second.Map; }

	auto MaterialBuildPrivate::FindShaderMapIdForRequest(FMaterialProgramIdentity RequestId,
		const MIR::FCompilerInput& Input, std::vector<FMaterialTranslationSource>& Sources) -> FMaterialProgramIdentity
	{
		std::lock_guard Lock(CacheMutex);
		const auto Entry = std::ranges::find_if(Programs, [&](const auto& Pair) { return Pair.second.RequestId == RequestId; });
		if (Entry == Programs.end()) return {};
		Sources.reserve(Entry->second.Sources.size());
		for (const auto& Index : Entry->second.Sources) {
			auto& Location = Sources.emplace_back(FMaterialTranslationSource{Input.Sources[Index.Ordinal], Index.Ordinal});
			Location.Source.ExpressionIndex = Index.ExpressionIndex;
		}
		return Entry->first;
	}

	auto MaterialBuildPrivate::RecordShaderMapRequest(FMaterialProgramIdentity RequestId, const FMaterialShaderMap& Map,
		std::span<const FMaterialTranslationSource> Sources) -> void
	{
		std::lock_guard Lock(CacheMutex);
		const auto Found = Programs.find(Map.ShaderMapId);
		if (Found == Programs.end() || Found->second.Map.get() != &Map) return;
		auto& Entry = Found->second;
		if (Entry.RequestId == RequestId) return;
		std::vector<FRetainedEntry::FSourceIndex> Indices;
		Indices.reserve(Sources.size());
		for (const auto& Location : Sources) Indices.push_back({Location.Ordinal, Location.Source.ExpressionIndex});
		const auto Size = Entry.MapBytes + Indices.capacity() * sizeof(FRetainedEntry::FSourceIndex);
		if (Size > MaxMapBytes || Bytes - Entry.Size() + Size > MaxCacheBytes) return;
		Bytes = Bytes - Entry.Size() + Size;
		Entry.Sources = std::move(Indices);
		Entry.RequestId = RequestId;
	}

	auto FMaterialShaderMap::Register(std::shared_ptr<const FMaterialShaderMap> ShaderMap, bool Replace)
		-> std::shared_ptr<const FMaterialShaderMap>
	{
		if (!ShaderMap || ShaderMap->CompiledShaders.empty()) return ShaderMap;
		const auto Size = ShaderMap->GetRetainedBytes(); if (Size > MaxMapBytes) return ShaderMap;
		std::lock_guard Lock(CacheMutex);
		const auto Found = Programs.find(ShaderMap->ShaderMapId);
		if (Found != Programs.end()) {
			if (!Replace) return Found->second.Map;
			Bytes -= Found->second.Size(); Programs.erase(Found); std::erase(Order, ShaderMap->ShaderMapId);
		}
		while (!Order.empty() && (Programs.size() >= 128 || Bytes + Size > MaxCacheBytes)) {
			const auto Old = Order.front(); Order.pop_front(); Bytes -= Programs.at(Old).Size(); Programs.erase(Old);
		}
		Bytes += Size; Programs.emplace(ShaderMap->ShaderMapId, FRetainedEntry{ShaderMap, Size}); Order.push_back(ShaderMap->ShaderMapId); return ShaderMap;
	}

	auto FMaterialShaderMap::ClearCache() -> void { std::lock_guard Lock(CacheMutex); Programs.clear(); Order.clear(); Bytes = 0; }
	auto FMaterialShaderMap::GetCacheStats() -> std::pair<uint32, uint64> { std::lock_guard Lock(CacheMutex); return {uint32(Programs.size()), Bytes}; }

}
