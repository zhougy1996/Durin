#pragma once
#include "CoreMinimal.h"
#include "Shader/ShaderCompilerCore.h"
#include "DerivedDataBuildOutput.h"

namespace Durin
{
	class FShaderOutputCache
	{
	public:
		auto Find(const std::string& Key, uint64 MaximumOutputBytes) -> std::optional<FShaderCompilerOutput>;
		auto Store(std::string Key, const FShaderCompilerOutput& Output, const DerivedData::FBuildOutput& Product) -> void;
		auto Count() const -> size_t;
	private:
		struct FEntry
		{
			FShaderCompilerOutput Output;
			DerivedData::FBuildOutput Product;
			std::list<std::string>::iterator Recency;
			uint64 Bytes = 0;
		};
		mutable std::mutex Mutex;
		std::list<std::string> Recency;
		std::unordered_map<std::string, FEntry> Entries;
		uint64 BytesRetained = 0;
	};
}
