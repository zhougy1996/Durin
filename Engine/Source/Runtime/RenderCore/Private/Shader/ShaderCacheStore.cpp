#include "Shader/ShaderCacheStore.h"

#include "Hash/XxHash.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Shader/ShaderCompiler.h"
#include "Shader/ShaderPaths.h"

namespace Durin
{
	namespace
	{
		auto HashBytes(std::span<const std::byte> Bytes) -> FXxHash64
		{
			return FXxHash64::HashBuffer(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		auto ToHex(FXxHash64 Hash) -> std::string
		{
			return std::format("{:016x}", Hash.HashValue);
		}

		auto ToHex(FXxHash128 Hash) -> std::string
		{
			return std::format("{:016x}{:016x}", Hash.HashHigh, Hash.HashLow);
		}

		auto ParseHex64(std::string_view Value, FXxHash64& OutHash) -> bool
		{
			try
			{
				OutHash.HashValue = static_cast<size_t>(std::stoull(std::string(Value), nullptr, 16));
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		auto ParseHex128(std::string_view Value, FXxHash128& OutHash) -> bool
		{
			if (Value.size() != 32)
			{
				return false;
			}
			try
			{
				OutHash.HashHigh = std::stoull(std::string(Value.substr(0, 16)), nullptr, 16);
				OutHash.HashLow = std::stoull(std::string(Value.substr(16, 16)), nullptr, 16);
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

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

		if (!ParseHex64(Root.GetStringValue("mainSourceHash"), OutMetaData.MainSourceHash)
			|| !ParseHex128(Root.GetStringValue("sourceTreeSignature"), OutMetaData.SourceTreeSignature))
		{
			return false;
		}

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
			if (Dependency.Path.empty() || !ParseHex64(DependencyView.GetStringValue("hash"), Dependency.ContentHash))
			{
				return false;
			}

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
			.AddFieldString("mainSourceHash", ToHex(MetaData.MainSourceHash))
			.AddFieldString("sourceTreeSignature", ToHex(MetaData.SourceTreeSignature));

		Writer.BeginArrayField("dependencies");
		for (const FShaderDependencyInfo& Dependency : MetaData.Dependencies)
		{
			Writer
				.BeginElementObject()
				.AddFieldString("path", Dependency.Path)
				.AddFieldUInt("size", Dependency.FileSize)
				.AddFieldString("hash", ToHex(Dependency.ContentHash))
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
			CompiledShader.Hash = HashBytes(*CompiledShader.Code);
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
