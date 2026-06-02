#include "Shader/ShaderCacheStore.h"

#include "Hash/XxHash.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/StringConvert.h"
#include "Shader/ShaderCompiler.h"
#include "Shader/ShaderPaths.h"

namespace Durin
{
	namespace
	{
		auto EntryPointToString(const char8* EntryPoint) -> std::string
		{
			return EntryPoint != nullptr ? std::string(EntryPoint) : std::string();
		}

		// Parser constants — must match what FJsonWriter emits.
		constexpr uint32 GShaderMetaVersion = 1;
		constexpr uint32 GShaderMacroSchemaVersion = 1;
		constexpr std::string_view GSlangBackendName = "slang";
		constexpr std::string_view GSlangTargetFormat = "SPIR-V";
		constexpr std::string_view GSlangTargetProfile = "spirv_1_5";
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
			|| Root.GetUIntValue("version") != GShaderMetaVersion
			|| Root.GetUIntValue("macroSchemaVersion") != GShaderMacroSchemaVersion
			|| Root.GetStringValue("backend") != GSlangBackendName
			|| Root.GetStringValue("targetFormat") != GSlangTargetFormat
			|| Root.GetStringValue("targetProfile") != GSlangTargetProfile
			|| Root.GetStringValue("virtualShaderPath") != VirtualShaderPath)
		{
			return false;
		}

		OutMetaData = {};
		OutMetaData.VirtualShaderPath = Root.GetStringValue("virtualShaderPath");

		const std::string MainSourceHash = Root.GetStringValue("mainSourceHash");
		const std::string SourceTreeSignature = Root.GetStringValue("sourceTreeSignature");
		if (!String::IsHex(MainSourceHash, 16)
			|| !String::IsHex(SourceTreeSignature, 32))
		{
			return false;
		}
		OutMetaData.MainSourceHash = FXxHash64::FromString(MainSourceHash);
		OutMetaData.SourceTreeSignature = FXxHash128::FromString(SourceTreeSignature);

		const FJsonValueView DependenciesView = Root.GetView("dependencies");
		if (!DependenciesView.IsArray())
		{
			return false;
		}

		OutMetaData.Dependencies.reserve(DependenciesView.Num());
		for (size_t DependencyIndex = 0; DependencyIndex < DependenciesView.Num(); ++DependencyIndex)
		{
			const FJsonValueView DependencyView = DependenciesView.GetView(DependencyIndex);
			if (!DependencyView.IsObject())
			{
				return false;
			}

			FShaderDependencyInfo Dependency;
			Dependency.Path = DependencyView.GetStringValue("path");
			Dependency.FileSize = DependencyView.GetUIntValue("size");
			const std::string DependencyHash = DependencyView.GetStringValue("hash");
			if (Dependency.Path.empty() || !String::IsHex(DependencyHash, 16))
			{
				return false;
			}
			Dependency.ContentHash = FXxHash64::FromString(DependencyHash);

			OutMetaData.Dependencies.push_back(std::move(Dependency));
		}

		return true;
	}

	auto FShaderCacheStore::SaveMetaData(const FShaderMetaData& MetaData) -> bool
	{
		FJsonWriter Writer;
		Writer
			.AddFieldUInt("version", GShaderMetaVersion)
			.AddFieldUInt("macroSchemaVersion", GShaderMacroSchemaVersion)
			.AddFieldString("virtualShaderPath", MetaData.VirtualShaderPath)
			.AddFieldString("backend", GSlangBackendName)
			.AddFieldString("targetFormat", GSlangTargetFormat)
			.AddFieldString("targetProfile", GSlangTargetProfile)
			.AddFieldString("mainSourceHash", MetaData.MainSourceHash.ToString())
			.AddFieldString("sourceTreeSignature", MetaData.SourceTreeSignature.ToString());

		Writer.BeginArrayField("dependencies");
		for (const FShaderDependencyInfo& Dependency : MetaData.Dependencies)
		{
			Writer
				.BeginElementObject()
				.AddFieldString("path", Dependency.Path)
				.AddFieldUInt("size", Dependency.FileSize)
				.AddFieldString("hash", Dependency.ContentHash.ToString())
				.EndNested();
		}
		Writer.EndNested();

		return Writer.SaveToFile(FShaderPaths::MetaPath(MetaData.VirtualShaderPath));
	}

	auto FShaderCacheStore::TryLoad(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, FShaderCompilerOutput& OutOutput) -> bool
	{
		const uint32 EntryPointCount = static_cast<uint32>(Options.EntryPoints.size());
		OutOutput.CompiledShaders.clear();
		OutOutput.CompiledShaders.resize(EntryPointCount);

		for (uint32 EntryPointIndex = 0; EntryPointIndex < EntryPointCount; ++EntryPointIndex)
		{
			const std::string EntryPoint = EntryPointToString(Options.EntryPoints[EntryPointIndex]);
			const std::string CachePath = FShaderPaths::BinaryPath(VirtualShaderPath, EntryPoint, Options.Frequencies[EntryPointIndex], VariantKey.Hex);

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

		for (uint32 EntryPointIndex = 0; EntryPointIndex < Output.CompiledShaders.size(); ++EntryPointIndex)
		{
			const FCompiledShader& CompiledShader = Output.CompiledShaders[EntryPointIndex];
			if (!CompiledShader.Code)
			{
				DURIN_WARN("Shader cache save skipped because compiled shader code is null.");
				return false;
			}

			const std::string EntryPoint = EntryPointToString(Options.EntryPoints[EntryPointIndex]);
			const std::string CachePath = FShaderPaths::BinaryPath(VirtualShaderPath, EntryPoint, Options.Frequencies[EntryPointIndex], VariantKey.Hex);
			if (!FFileHelper::SaveArrayToFile(*CompiledShader.Code, CachePath))
			{
				DURIN_WARN("Failed to write shader cache artifact: {}", CachePath);
				return false;
			}
		}

		return true;
	}
}
