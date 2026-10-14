#include "ShaderOutputCache.h"

namespace Durin
{
	auto FShaderOutputCache::Find(const std::string& Key, uint64 MaximumOutputBytes) -> std::optional<FShaderCompilerOutput>
	{
		std::lock_guard Lock(Mutex);
		const auto Found = Entries.find(Key);
		if (Found == Entries.end()) return {};
		Recency.splice(Recency.begin(), Recency, Found->second.Recency);
		if (!Found->second.Product.CheckLimits({.MaximumTotalBytes = MaximumOutputBytes}))
			return FShaderCompilerOutput{.Error = {.Code = EShaderError::CaptureInputLimit}};
		return Found->second.Output;
	}
	auto FShaderOutputCache::Count() const -> size_t
	{ std::lock_guard Lock(Mutex); return Entries.size(); }

	auto FShaderOutputCache::Store(std::string Key, const FShaderCompilerOutput& Output, const DerivedData::FBuildOutput& Product) -> void
	{
		uint64 Bytes = sizeof(FEntry) + 2 * Key.capacity() + Output.CompiledShaders.capacity() * sizeof(FCompiledShader);
		for (const auto& Meta : Product.GetMetadata()) Bytes += sizeof(Meta) + Meta.Object.GetBytes().GetSize();
		for (const auto& Value : Product.GetValues()) Bytes += sizeof(Value) + Value.Value.GetRawSize();
		for (const auto& Observation : Product.GetObservations()) Bytes += sizeof(Observation) + Observation.Object.GetBytes().GetSize();
		for (const auto& Message : Product.GetMessages()) Bytes += sizeof(Message) + Message.Text.capacity();
		for (const auto& Log : Product.GetLogs()) Bytes += sizeof(Log) + Log.Category.capacity() + Log.Text.capacity();
		for (const auto& Shader : Output.CompiledShaders)
		{
			Bytes += Shader.Reflection.ResourceBindings.capacity() * sizeof(FShaderResourceBinding)
				+ Shader.Reflection.PushConstantRanges.capacity() * sizeof(FPushConstantRange)
				+ Shader.MetalBindings.capacity() * sizeof(FMetalShaderBinding)
				+ Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity();
			for (const auto& Binding : Shader.Reflection.ResourceBindings) Bytes += Binding.Name.capacity();
		}
		if (Bytes > 256ull * 1024 * 1024) return;
		std::lock_guard Lock(Mutex);
		if (auto FoundIt = Entries.find(Key); FoundIt != Entries.end())
		{
			BytesRetained -= FoundIt->second.Bytes;
			BytesRetained += Bytes;
			FoundIt->second.Bytes = Bytes;
			FoundIt->second.Output = Output; FoundIt->second.Product = Product;
			Recency.splice(Recency.begin(), Recency, FoundIt->second.Recency);
		}
		else
		{
			Recency.push_front(std::move(Key));
			Entries.emplace(Recency.front(), FEntry{Output, Product, Recency.begin(), Bytes});
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
