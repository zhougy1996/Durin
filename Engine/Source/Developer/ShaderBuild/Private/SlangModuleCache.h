#pragma once

#include "CoreMinimal.h"
#include "Shader/ShaderCompilerCore.h"
#include "slang.h"

namespace Durin
{
	// Owned by one compiler service. Only immutable serialized modules cross session/thread boundaries.
	class FSlangModuleCache
	{
	public:
		using FModules = std::map<std::string, FSharedByteBuffer>;
		static auto MakeKey(const FShaderCompileOptions& Options, std::string_view SearchPath,
			std::string_view GeneratedSource) -> std::optional<FXxHash128>;
		auto Find(FXxHash128 Key) const -> FModules;
		auto Store(FXxHash128 Key, slang::ISession& Session, slang::IModule& Root,
			const FShaderSourceArtifacts& Sources, const FModules& Offered) -> void;
	private:
		struct FEntry
		{
			FXxHash128 Key;
			std::string Path;
			FSharedByteBuffer IR;
		};
		mutable std::mutex Mutex;
		std::list<FEntry> Entries;
		uint64 RetainedBytes = 0;
	};
}
