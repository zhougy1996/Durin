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

		auto AreBinaryCachePathsUnique(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey) -> bool
		{
			std::unordered_set<std::string> CachePaths;
			CachePaths.reserve(Options.EntryPoints.size());

			for (const char8* EntryPoint : Options.EntryPoints)
			{
				const std::string CachePath = FShaderPaths::BinaryPath(VirtualShaderPath, EntryPointToString(EntryPoint), VariantKey.Hex);
				if (!CachePaths.insert(CachePath).second)
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
		FJsonDocument Document;
		if (!Document.LoadFromFile(FShaderPaths::MetaPath(VirtualShaderPath)))
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
		if (Options.EntryPoints.size() != Options.Frequencies.size())
		{
			DURIN_WARN("Shader cache load skipped because entry point count does not match shader frequency count.");
			return false;
		}
		if (!AreBinaryCachePathsUnique(VirtualShaderPath, Options, VariantKey))
		{
			return false;
		}

		OutOutput.CompiledShaders.clear();
		OutOutput.CompiledShaders.resize(EntryPointCount);

		for (uint32 EntryPointIndex = 0; EntryPointIndex < EntryPointCount; ++EntryPointIndex)
		{
			const std::string EntryPoint = EntryPointToString(Options.EntryPoints[EntryPointIndex]);
			const std::string CachePath = FShaderPaths::BinaryPath(VirtualShaderPath, EntryPoint, VariantKey.Hex);

			std::vector<uint8> ShaderBytes;
			if (!FFileHelper::LoadFileToArray(ShaderBytes, CachePath))
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
		if (Options.EntryPoints.size() != Options.Frequencies.size())
		{
			DURIN_WARN("Shader cache save skipped because entry point count does not match shader frequency count.");
			return false;
		}
		if (!AreBinaryCachePathsUnique(VirtualShaderPath, Options, VariantKey))
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

			const std::string EntryPoint = EntryPointToString(Options.EntryPoints[EntryPointIndex]);
			const std::string CachePath = FShaderPaths::BinaryPath(VirtualShaderPath, EntryPoint, VariantKey.Hex);
			if (!FFileHelper::SaveArrayToFile(*CompiledShader.Code, CachePath))
			{
				DURIN_WARN("Failed to write shader cache artifact: {}", CachePath);
				return false;
			}
		}

		return true;
	}
}
