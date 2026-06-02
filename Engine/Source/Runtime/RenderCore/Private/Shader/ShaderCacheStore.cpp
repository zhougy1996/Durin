#include "Shader/ShaderCacheStore.h"

#include "Hash/XxHash.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/StringConvert.h"
#include "Shader/ShaderCompiler.h"
#include "Shader/ShaderPaths.h"

#include <unordered_set>

namespace Durin
{
	namespace
	{
		auto EntryPointToString(const char8* EntryPoint) -> std::string
		{
			return EntryPoint != nullptr ? std::string(EntryPoint) : std::string();
		}

		auto ValidateEntryPointCounts(const FShaderCompileOptions& Options, std::string_view OperationName) -> bool
		{
			if (Options.EntryPoints.size() != Options.Frequencies.size())
			{
				DURIN_WARN("Shader cache {} skipped because entry point count does not match shader frequency count.", OperationName);
				return false;
			}

			return true;
		}

		auto BuildBinaryCachePaths(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, std::vector<std::string>& OutCachePaths) -> bool
		{
			OutCachePaths.clear();
			OutCachePaths.reserve(Options.EntryPoints.size());

			std::unordered_set<std::string> UniqueCachePaths;
			UniqueCachePaths.reserve(Options.EntryPoints.size());

			for (const char8* EntryPoint : Options.EntryPoints)
			{
				OutCachePaths.push_back(FShaderPaths::BinaryPath(VirtualShaderPath, EntryPointToString(EntryPoint), VariantKey.Hex));
				const std::string& CachePath = OutCachePaths.back();
				if (!UniqueCachePaths.insert(CachePath).second)
				{
					DURIN_WARN("Shader binary cache skipped because multiple entry points map to the same file: {}", CachePath);
					return false;
				}
			}

			return true;
		}

		// Parser constants — must match what FJsonWriter emits.
		constexpr uint32 GShaderMetaVersion = 2;
	}

	FShaderCacheStore::FShaderCacheStore() = default;
	FShaderCacheStore::~FShaderCacheStore() = default;

	auto FShaderCacheStore::LoadMetaData(std::string_view VirtualShaderPath, FShaderMetaData& OutMetaData) -> bool
	{
		const std::string MetaPath = FShaderPaths::MetaPath(VirtualShaderPath);
		if (!FFileHelper::FileExists(MetaPath))
		{
			return false;
		}

		FJsonDocument Document;
		if (!Document.LoadFromFile(MetaPath))
		{
			return false;
		}

		const FJsonValueView Root = Document.GetRootView();
		if (!Root.IsObject()
			|| Root.GetUIntValue("Version") != GShaderMetaVersion)
		{
			return false;
		}

		OutMetaData = {};

		const std::string SourceTreeSignature = Root.GetStringValue("SourceTreeSignature");
		if (!String::IsHex(SourceTreeSignature, 32))
		{
			return false;
		}
		OutMetaData.SourceTreeSignature = FXxHash128::FromString(SourceTreeSignature);

		return true;
	}

	auto FShaderCacheStore::SaveMetaData(std::string_view VirtualShaderPath, const FShaderMetaData& MetaData) -> bool
	{
		FJsonWriter Writer;
		Writer
			.AddFieldUInt("Version", GShaderMetaVersion)
			.AddFieldString("SourceTreeSignature", MetaData.SourceTreeSignature.ToString());

		return Writer.SaveToFile(FShaderPaths::MetaPath(VirtualShaderPath));
	}

	auto FShaderCacheStore::TryLoad(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, FShaderCompilerOutput& OutOutput) -> bool
	{
		const uint32 EntryPointCount = static_cast<uint32>(Options.EntryPoints.size());
		if (!ValidateEntryPointCounts(Options, "load"))
		{
			return false;
		}

		std::vector<std::string> CachePaths;
		if (!BuildBinaryCachePaths(VirtualShaderPath, Options, VariantKey, CachePaths))
		{
			return false;
		}

		OutOutput.CompiledShaders.clear();
		OutOutput.CompiledShaders.resize(EntryPointCount);

		for (uint32 EntryPointIndex = 0; EntryPointIndex < EntryPointCount; ++EntryPointIndex)
		{
			std::vector<uint8> ShaderBytes;
			if (!FFileHelper::LoadFileToArray(ShaderBytes, CachePaths[EntryPointIndex]))
			{
				return false;
			}

			auto& CompiledShader = OutOutput.CompiledShaders[EntryPointIndex];
			CompiledShader.Frequency = Options.Frequencies[EntryPointIndex];
			CompiledShader.Code = std::make_shared<FShaderCode>();
			CompiledShader.Code->resize(ShaderBytes.size());
			if (!ShaderBytes.empty())
			{
				std::memcpy(CompiledShader.Code->data(), ShaderBytes.data(), ShaderBytes.size());
			}
			CompiledShader.Hash = FXxHash64::HashBuffer(*CompiledShader.Code);
		}

		OutOutput.bSucceeded = true;
		return true;
	}

	auto FShaderCacheStore::Save(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, const FShaderCompilerOutput& Output) -> bool
	{
		if (Output.CompiledShaders.size() != Options.EntryPoints.size())
		{
			DURIN_WARN("Shader cache save skipped because compiler output count does not match requested entry point count.");
			return false;
		}
		if (!ValidateEntryPointCounts(Options, "save"))
		{
			return false;
		}

		std::vector<std::string> CachePaths;
		if (!BuildBinaryCachePaths(VirtualShaderPath, Options, VariantKey, CachePaths))
		{
			return false;
		}

		for (uint32 EntryPointIndex = 0; EntryPointIndex < Output.CompiledShaders.size(); ++EntryPointIndex)
		{
			const FCompiledShader& CompiledShader = Output.CompiledShaders[EntryPointIndex];
			if (!CompiledShader.Code)
			{
				DURIN_WARN("Shader cache save skipped because compiled shader code is null.");
				return false;
			}

			if (!FFileHelper::SaveArrayToFile(*CompiledShader.Code, CachePaths[EntryPointIndex]))
			{
				DURIN_WARN("Failed to write shader cache artifact: {}", CachePaths[EntryPointIndex]);
				return false;
			}
		}

		return true;
	}
}
