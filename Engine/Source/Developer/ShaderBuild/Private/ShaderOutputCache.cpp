#include "ShaderOutputCache.h"

namespace Durin
{
	auto FShaderOutputCache::Find(const std::string& Key) -> std::optional<FShaderCompilerOutput>
	{
		std::lock_guard Lock(Mutex);
		const auto Found = Entries.find(Key);
		if (Found == Entries.end()) return {};
		Recency.splice(Recency.begin(), Recency, Found->second.Recency);
		return Found->second.Output;
	}
	auto FShaderOutputCache::Count() const -> size_t
	{ std::lock_guard Lock(Mutex); return Entries.size(); }

	auto FShaderOutputCache::Store(std::string Key, const FShaderCompilerOutput& Output) -> void
	{
		uint64 Bytes = sizeof(Output) + Key.capacity();
		for (const auto& Shader : Output.CompiledShaders)
		{
			Bytes += sizeof(Shader) + (Shader.Code ? Shader.Code->size() : 0)
				+ Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity();
			for (const auto& Binding : Shader.Reflection.ResourceBindings) Bytes += sizeof(Binding) + Binding.Name.capacity();
		}
		if (Bytes > 256ull * 1024 * 1024) return;
		std::lock_guard Lock(Mutex);
		if (auto FoundIt = Entries.find(Key); FoundIt != Entries.end())
		{
			BytesRetained -= FoundIt->second.Bytes;
			BytesRetained += Bytes;
			FoundIt->second.Bytes = Bytes;
			FoundIt->second.Output = Output;
			Recency.splice(Recency.begin(), Recency, FoundIt->second.Recency);
		}
		else
		{
			Recency.push_front(std::move(Key));
			Entries.emplace(Recency.front(), FEntry{Output, Recency.begin(), Bytes});
			BytesRetained += Bytes;
		}
		while (Entries.size() > 128 || BytesRetained > 256ull * 1024 * 1024)
		{
			BytesRetained -= Entries.at(Recency.back()).Bytes;
			Entries.erase(Recency.back());
			Recency.pop_back();
		}
	}

}
