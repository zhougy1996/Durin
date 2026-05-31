#include "SlangShaderCompiler.h"

#include "Hash/XxHash.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Shader/ShaderPaths.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 GShaderMetaVersion = 1;
		constexpr uint32 GShaderMacroSchemaVersion = 1;
		constexpr std::string_view GShaderVariantKeyVersion = "DurinShaderVariantKey_v3";
		constexpr std::string_view GSlangBackendName = "slang";
		constexpr std::string_view GSlangTargetFormat = "SPIR-V";
		constexpr std::string_view GSlangTargetProfile = "spirv_1_5";

		auto ToHex(FXxHash64 Hash) -> std::string
		{
			return std::format("{:016x}", Hash.HashValue);
		}

		auto ToHex(FXxHash128 Hash) -> std::string
		{
			return std::format("{:016x}{:016x}", Hash.HashHigh, Hash.HashLow);
		}

		auto EntryPointToString(const char8* EntryPoint) -> std::string
		{
			return EntryPoint != nullptr ? std::string(EntryPoint) : std::string();
		}

		auto FrequencyToInt(EShaderFrequency Frequency) -> int32
		{
			return static_cast<int32>(Frequency);
		}

		auto HashShaderCode(FShaderCodeView Code) -> FXxHash64
		{
			return FXxHash64::HashBuffer(Code.empty() ? nullptr : Code.data(), static_cast<uint64>(Code.size_bytes()));
		}

		auto HashBytes(std::span<const uint8> Bytes) -> FXxHash64
		{
			return FXxHash64::HashBuffer(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		auto EscapeJsonString(std::string_view Value) -> std::string
		{
			std::string Result;
			Result.reserve(Value.size() + 8);
			for (const char Char : Value)
			{
				switch (Char)
				{
				case '\\':
					Result += "\\\\";
					break;
				case '"':
					Result += "\\\"";
					break;
				case '\b':
					Result += "\\b";
					break;
				case '\f':
					Result += "\\f";
					break;
				case '\n':
					Result += "\\n";
					break;
				case '\r':
					Result += "\\r";
					break;
				case '\t':
					Result += "\\t";
					break;
				default:
					if (static_cast<unsigned char>(Char) < 0x20)
					{
						Result += std::format("\\u{:04x}", static_cast<unsigned char>(Char));
					}
					else
					{
						Result.push_back(Char);
					}
					break;
				}
			}
			return Result;
		}

		auto AppendJsonStringField(std::string& Json, std::string_view Key, std::string_view Value, bool bTrailingComma) -> void
		{
			Json += std::format("  \"{}\": \"{}\"", Key, EscapeJsonString(Value));
			Json += bTrailingComma ? ",\n" : "\n";
		}

		auto AppendJsonUIntField(std::string& Json, std::string_view Key, uint64 Value, bool bTrailingComma) -> void
		{
			Json += std::format("  \"{}\": {}", Key, Value);
			Json += bTrailingComma ? ",\n" : "\n";
		}

		auto UpdateString(FXxHash128Builder& Builder, std::string_view Value) -> void
		{
			const uint64 Length = static_cast<uint64>(Value.size());
			Builder.Update(&Length, sizeof(Length));
			if (Length > 0)
			{
				Builder.Update(Value.data(), Length);
			}
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
	}

	FSlangShaderCompiler::FSlangShaderCompiler()
	{
		InitGlobalSession();
	}

	FSlangShaderCompiler::~FSlangShaderCompiler()
	{
	}

	auto FSlangShaderCompiler::NormalizePath(const std::filesystem::path& InPath) const -> std::string
	{
		std::error_code ErrorCode;
		const std::filesystem::path CanonicalPath = std::filesystem::weakly_canonical(InPath, ErrorCode);
		if (!ErrorCode)
		{
			return CanonicalPath.generic_string();
		}
		return InPath.lexically_normal().generic_string();
	}

	auto FSlangShaderCompiler::TryMakeShaderVirtualPath(std::string_view PhysicalSourcePath, std::string& OutVirtualSourcePath) const -> bool
	{
		return FShaderPaths::TryMakeVirtualSourcePath(PhysicalSourcePath, OutVirtualSourcePath);
	}

	auto FSlangShaderCompiler::NormalizeMacros(const FShaderCompileOptions& Options, std::vector<FShaderMacroDefinition>& OutMacros, std::string& OutErrorMessage) const -> bool
	{
		OutMacros = Options.Macros;
		// Normalize macro ordering before both hashing and Slang session creation so cache keys and compiler inputs stay aligned.
		std::ranges::sort(OutMacros, [](const FShaderMacroDefinition& A, const FShaderMacroDefinition& B) {
			if (A.Name != B.Name)
			{
				return A.Name < B.Name;
			}
			if (A.Value != B.Value)
			{
				return A.Value < B.Value;
			}
			return A.bHasExplicitValue < B.bHasExplicitValue;
		});

		for (size_t Index = 1; Index < OutMacros.size(); ++Index)
		{
			if (OutMacros[Index - 1].Name == OutMacros[Index].Name)
			{
				OutErrorMessage = std::format("Duplicate shader macro definition is not allowed: {}", OutMacros[Index].Name);
				return false;
			}
		}

		return true;
	}

	auto FSlangShaderCompiler::CreateSession(const FShaderCompileOptions& Options, Slang::ComPtr<slang::ISession>& OutSession, std::string& OutErrorMessage) const -> bool
	{
		std::vector<FShaderMacroDefinition> NormalizedMacros;
		if (!NormalizeMacros(Options, NormalizedMacros, OutErrorMessage))
		{
			return false;
		}

		std::vector<slang::PreprocessorMacroDesc> SlangMacros;
		SlangMacros.reserve(NormalizedMacros.size());
		for (const FShaderMacroDefinition& Macro : NormalizedMacros)
		{
			slang::PreprocessorMacroDesc MacroDesc = {};
			MacroDesc.name = Macro.Name.c_str();
			MacroDesc.value = Macro.bHasExplicitValue ? Macro.Value.c_str() : "1";
			SlangMacros.push_back(MacroDesc);
		}

		slang::TargetDesc TargetDesc = {};
		TargetDesc.format = SLANG_SPIRV;
		TargetDesc.profile = GlobalSession->findProfile(GSlangTargetProfile.data());

		slang::SessionDesc SessionDesc = {};
		SessionDesc.targets = &TargetDesc;
		SessionDesc.targetCount = 1;
		SessionDesc.preprocessorMacros = SlangMacros.empty() ? nullptr : SlangMacros.data();
		SessionDesc.preprocessorMacroCount = static_cast<SlangInt>(SlangMacros.size());

		if (SLANG_FAILED(GlobalSession->createSession(SessionDesc, OutSession.writeRef())))
		{
			OutErrorMessage = "createSession failed";
			return false;
		}

		return true;
	}

	auto FSlangShaderCompiler::ResolveDependencyFiles(
		slang::ISession* InSession,
		const char8* InShaderFilePath,
		std::vector<std::string>& OutDependencyPaths,
		std::string& OutDiagnostics
	) const -> Slang::Result
	{
		Slang::ComPtr<slang::IBlob> DiagnosticsBlob;
		slang::IModule* Module = InSession->loadModule(InShaderFilePath, DiagnosticsBlob.writeRef());
		if (!Module)
		{
			if (DiagnosticsBlob)
			{
				OutDiagnostics = static_cast<const char*>(DiagnosticsBlob->getBufferPointer());
			}
			return SLANG_FAIL;
		}

		OutDependencyPaths.clear();
		OutDependencyPaths.push_back(NormalizePath(std::filesystem::path(InShaderFilePath)));

		const SlangInt DependencyCount = Module->getDependencyFileCount();
		for (SlangInt DependencyIndex = 0; DependencyIndex < DependencyCount; ++DependencyIndex)
		{
			const char* DependencyPath = Module->getDependencyFilePath(DependencyIndex);
			if (DependencyPath && DependencyPath[0] != '\0')
			{
				OutDependencyPaths.push_back(NormalizePath(std::filesystem::path(DependencyPath)));
			}
		}

		std::ranges::sort(OutDependencyPaths);
		const auto UniqueEnd = std::ranges::unique(OutDependencyPaths).begin();
		OutDependencyPaths.erase(UniqueEnd, OutDependencyPaths.end());
		return SLANG_OK;
	}

	auto FSlangShaderCompiler::BuildShaderMetaData(
		std::string_view VirtualShaderPath,
		std::string_view ShaderSourceFilePath,
		const std::vector<std::string>& InDependencyPaths,
		FShaderMetaData& OutMetaData,
		std::string& OutErrorMessage
	) const -> bool
	{
		OutMetaData = {};
		OutMetaData.VirtualShaderPath = std::string(VirtualShaderPath);
		const std::string NormalizedSourcePath = NormalizePath(std::filesystem::path(std::string(ShaderSourceFilePath)));
		std::string ResolvedVirtualShaderPath;
		if (!TryMakeShaderVirtualPath(NormalizedSourcePath, ResolvedVirtualShaderPath))
		{
			DURIN_WARN("Failed to map shader source path to a virtual shader path, falling back to normalized path: {}", NormalizedSourcePath);
			ResolvedVirtualShaderPath = NormalizedSourcePath;
		}
		OutMetaData.VirtualShaderPath = std::move(ResolvedVirtualShaderPath);
		OutMetaData.Dependencies.reserve(InDependencyPaths.size());

		FXxHash128Builder TreeSignatureBuilder;
		// The source tree signature tracks the full dependency graph content, not just the root .slang file.
		UpdateString(TreeSignatureBuilder, GShaderVariantKeyVersion);

		for (size_t DependencyIndex = 0; DependencyIndex < InDependencyPaths.size(); ++DependencyIndex)
		{
			const std::string& DependencyPath = InDependencyPaths[DependencyIndex];
			std::vector<uint8> FileBytes;
			if (!FFileHelper::LoadFileToArray(FileBytes, DependencyPath))
			{
				OutErrorMessage = std::format("Failed to read shader dependency file: {}", DependencyPath);
				return false;
			}

			FShaderDependencyInfo Dependency;
			if (!TryMakeShaderVirtualPath(DependencyPath, Dependency.Path))
			{
				Dependency.Path = DependencyPath;
				DURIN_WARN("Failed to map shader dependency path to a virtual shader path, falling back to normalized path: {}", DependencyPath);
			}
			Dependency.FileSize = static_cast<uint64>(FileBytes.size());
			Dependency.ContentHash = HashBytes(FileBytes);
			if (DependencyIndex == 0)
			{
				OutMetaData.MainSourceHash = Dependency.ContentHash;
			}

			UpdateString(TreeSignatureBuilder, Dependency.Path);
			TreeSignatureBuilder.Update(&Dependency.FileSize, sizeof(Dependency.FileSize));
			TreeSignatureBuilder.Update(&Dependency.ContentHash.HashValue, sizeof(Dependency.ContentHash.HashValue));

			OutMetaData.Dependencies.push_back(Dependency);
		}

		OutMetaData.SourceTreeSignature = TreeSignatureBuilder.Finalize();
		return true;
	}

	auto FSlangShaderCompiler::BuildVariantKey(
		const FShaderMetaData& MetaData,
		const std::vector<FShaderMacroDefinition>& Macros,
		FShaderVariantKey& OutVariantKey
	) const -> void
	{
		FXxHash128Builder Builder;
		UpdateString(Builder, GShaderVariantKeyVersion);
		UpdateString(Builder, GSlangBackendName);
		UpdateString(Builder, GSlangTargetFormat);
		UpdateString(Builder, GSlangTargetProfile);
		UpdateString(Builder, MetaData.VirtualShaderPath);
		Builder.Update(&MetaData.SourceTreeSignature.HashLow, sizeof(MetaData.SourceTreeSignature.HashLow));
		Builder.Update(&MetaData.SourceTreeSignature.HashHigh, sizeof(MetaData.SourceTreeSignature.HashHigh));

		// Entry point and frequency do not participate in the variant directory hash. They are separated at the artifact file name level.
		const uint64 MacroCount = static_cast<uint64>(Macros.size());
		Builder.Update(&MacroCount, sizeof(MacroCount));
		for (const FShaderMacroDefinition& Macro : Macros)
		{
			UpdateString(Builder, Macro.Name);
			UpdateString(Builder, Macro.Value);
			Builder.Update(&Macro.bHasExplicitValue, sizeof(Macro.bHasExplicitValue));
		}

		OutVariantKey.Value = Builder.Finalize();
		OutVariantKey.Hex = ToHex(OutVariantKey.Value);
	}

	auto FSlangShaderCompiler::LoadMetaData(std::string_view VirtualShaderPath, FShaderMetaData& OutMetaData) const -> bool
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

	auto FSlangShaderCompiler::IsMetaDataCurrent(const FShaderMetaData& CurrentMetaData, const FShaderMetaData& CachedMetaData) const -> bool
	{
		if (CurrentMetaData.VirtualShaderPath != CachedMetaData.VirtualShaderPath
			|| CurrentMetaData.MainSourceHash != CachedMetaData.MainSourceHash
			|| CurrentMetaData.SourceTreeSignature != CachedMetaData.SourceTreeSignature
			|| CurrentMetaData.Dependencies.size() != CachedMetaData.Dependencies.size())
		{
			return false;
		}

		for (size_t DependencyIndex = 0; DependencyIndex < CurrentMetaData.Dependencies.size(); ++DependencyIndex)
		{
			const FShaderDependencyInfo& CurrentDependency = CurrentMetaData.Dependencies[DependencyIndex];
			const FShaderDependencyInfo& CachedDependency = CachedMetaData.Dependencies[DependencyIndex];
			if (CurrentDependency.Path != CachedDependency.Path
				|| CurrentDependency.FileSize != CachedDependency.FileSize
				|| CurrentDependency.ContentHash != CachedDependency.ContentHash)
			{
				return false;
			}
		}

		return true;
	}

	auto FSlangShaderCompiler::TryLoadShaderCache(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		const FShaderVariantKey& VariantKey,
		FShaderCompilerOutput& OutOutput
	) const -> bool
	{
		const uint32 EntryPointCount = static_cast<uint32>(Options.EntryPoints.size());
		OutOutput.CompiledShaders.clear();
		OutOutput.CompiledShaders.resize(EntryPointCount);

		// Meta validation happens before this path. Here we only verify the expected per-entry artifact still exists and can be loaded.
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
			CompiledShader.Hash = HashBytes(ShaderBytes);
		}

		OutOutput.bSucceeded = true;
		return true;
	}

	auto FSlangShaderCompiler::SaveMetaData(const FShaderMetaData& MetaData) const -> bool
	{
		std::string Json;
		Json.reserve(1024 + MetaData.Dependencies.size() * 96);
		Json += "{\n";
		AppendJsonUIntField(Json, "version", GShaderMetaVersion, true);
		AppendJsonUIntField(Json, "macroSchemaVersion", GShaderMacroSchemaVersion, true);
		AppendJsonStringField(Json, "virtualShaderPath", MetaData.VirtualShaderPath, true);
		AppendJsonStringField(Json, "backend", GSlangBackendName, true);
		AppendJsonStringField(Json, "targetFormat", GSlangTargetFormat, true);
		AppendJsonStringField(Json, "targetProfile", GSlangTargetProfile, true);
		AppendJsonStringField(Json, "mainSourceHash", ToHex(MetaData.MainSourceHash), true);
		AppendJsonStringField(Json, "sourceTreeSignature", ToHex(MetaData.SourceTreeSignature), true);
		Json += "  \"dependencies\": [\n";
		for (size_t DependencyIndex = 0; DependencyIndex < MetaData.Dependencies.size(); ++DependencyIndex)
		{
			const FShaderDependencyInfo& Dependency = MetaData.Dependencies[DependencyIndex];
			Json += "    {\n";
			Json += std::format("      \"path\": \"{}\",\n", EscapeJsonString(Dependency.Path));
			Json += std::format("      \"size\": {},\n", Dependency.FileSize);
			Json += std::format("      \"hash\": \"{}\"\n", ToHex(Dependency.ContentHash));
			Json += DependencyIndex + 1 < MetaData.Dependencies.size() ? "    },\n" : "    }\n";
		}
		Json += "  ]\n";
		Json += "}\n";

		const std::string MetaPath = FShaderPaths::MetaPath(MetaData.VirtualShaderPath);
		const std::span<const std::byte> Bytes(
			reinterpret_cast<const std::byte*>(Json.data()),
			Json.size()
		);
		return FFileHelper::SaveArrayToFile(Bytes, MetaPath);
	}

	auto FSlangShaderCompiler::SaveCompiledShaderCache(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		const FShaderVariantKey& VariantKey,
		const FShaderCompilerOutput& Output
	) const -> bool
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

	static auto ConvertBlobToArray(const Slang::ComPtr<slang::IBlob>& FromBlob, FShaderCode& OutCode) -> bool
	{
		const void* BufferPtr = FromBlob->getBufferPointer();
		const size_t BufferSize = FromBlob->getBufferSize();

		if (BufferSize == 0 || BufferSize % sizeof(uint32) != 0)
		{
			DURIN_ERROR("Invalid SPIR-V size: {} bytes", BufferSize);
			return false;
		}

		OutCode.clear();
		OutCode.resize(BufferSize);
		std::memcpy(OutCode.data(), BufferPtr, BufferSize);
		return true;
	}

	static auto FillCompilerOutput(Slang::ComPtr<slang::IComponentType>& ComposedProgram, const FShaderCompileOptions& Options, FShaderCompilerOutput& Output) -> Slang::Result
	{
		const uint32 EntryPointCount = static_cast<uint32>(Options.EntryPoints.size());
		Output.CompiledShaders.resize(EntryPointCount);
		for (uint32 Index = 0; Index < EntryPointCount; ++Index)
		{
			Slang::ComPtr<slang::IBlob> CodeBlob;
			SLANG_RETURN_ON_FAIL(ComposedProgram->getEntryPointCode(Index, 0, CodeBlob.writeRef(), nullptr));

			auto& CompiledShader = Output.CompiledShaders[Index];
			CompiledShader.Frequency = Options.Frequencies[Index];
			CompiledShader.Code = std::make_shared<FShaderCode>();
			if (!ConvertBlobToArray(CodeBlob, *CompiledShader.Code))
			{
				return SLANG_FAIL;
			}
			CompiledShader.Hash = HashShaderCode(*CompiledShader.Code);
		}
		return SLANG_OK;
	}

	FShaderCompilerOutput FSlangShaderCompiler::Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options)
	{
		FShaderCompilerOutput Output;

		const auto& EntryPoints = Options.EntryPoints;
		const uint32 EntryPointCount = static_cast<uint32>(EntryPoints.size());
		if (EntryPointCount == 0)
		{
			Output.ErrorMessage = "No entry points found";
			return Output;
		}

		if (EntryPointCount != Options.Frequencies.size())
		{
			Output.ErrorMessage = "Entry point count does not match shader frequency count";
			return Output;
		}

		std::vector<FShaderMacroDefinition> NormalizedMacros;
		if (!NormalizeMacros(Options, NormalizedMacros, Output.ErrorMessage))
		{
			return Output;
		}

		const bool bForceRecompile = Options.bForceRecompile || Settings.bForceRecompile;
		const std::string SourceFilePath(ShaderSourceFilePath);
		// Callers should pass a virtual path, but we still fall back to the physical source path so ad hoc compiles remain usable.
		const std::string VirtualShaderPath = !Options.VirtualShaderPath.empty() ? Options.VirtualShaderPath : SourceFilePath;

		Slang::ComPtr<slang::ISession> CompileSession;
		if (!CreateSession(Options, CompileSession, Output.ErrorMessage))
		{
			return Output;
		}

		std::vector<std::string> DependencyPaths;
		std::string DependencyDiagnostics;
		if (SLANG_FAILED(ResolveDependencyFiles(CompileSession.get(), SourceFilePath.data(), DependencyPaths, DependencyDiagnostics)))
		{
			Output.ErrorMessage = DependencyDiagnostics.empty() ? "Failed to parse shader dependency graph" : DependencyDiagnostics;
			return Output;
		}

		FShaderMetaData CurrentMetaData;
		if (!BuildShaderMetaData(VirtualShaderPath, SourceFilePath, DependencyPaths, CurrentMetaData, Output.ErrorMessage))
		{
			return Output;
		}

		FShaderVariantKey VariantKey;
		BuildVariantKey(CurrentMetaData, NormalizedMacros, VariantKey);

		FShaderMetaData CachedMetaData;
		const bool bMetaDataCurrent = LoadMetaData(VirtualShaderPath, CachedMetaData) && IsMetaDataCurrent(CurrentMetaData, CachedMetaData);
		if (!bForceRecompile && bMetaDataCurrent && TryLoadShaderCache(VirtualShaderPath, Options, VariantKey, Output))
		{
			return Output;
		}

		Slang::ComPtr<slang::IBlob> DiagnosticsBlob;
		Slang::ComPtr<slang::IComponentType> ComposedProgram;
		const Slang::Result CompileResult = CompileInternal(CompileSession.get(), SourceFilePath.data(), EntryPoints, ComposedProgram, DiagnosticsBlob);

		if (SLANG_FAILED(CompileResult))
		{
			if (DiagnosticsBlob != nullptr)
			{
				Output.ErrorMessage = std::string{"Failed to compile shader. Diagnostics: \n"} + static_cast<const char*>(DiagnosticsBlob->getBufferPointer());
			}
			return Output;
		}

		if (SLANG_FAILED(FillCompilerOutput(ComposedProgram, Options, Output)))
		{
			Output.ErrorMessage = "Failed to fill shader compiler output";
			return Output;
		}

		if (!SaveCompiledShaderCache(VirtualShaderPath, Options, VariantKey, Output))
		{
			DURIN_WARN("Shader compiled successfully, but cache write failed for {}", VirtualShaderPath);
		}
		if (!SaveMetaData(CurrentMetaData))
		{
			DURIN_WARN("Shader compiled successfully, but meta write failed for {}", VirtualShaderPath);
		}

		Output.bSucceeded = true;
		return Output;
	}

	auto FSlangShaderCompiler::CompileInternal(
		slang::ISession* InSession,
		const char8* InShaderFilePath,
		const std::span<const char8* const>& InEntryPoints,
		Slang::ComPtr<slang::IComponentType>& OutComposedProgram,
		Slang::ComPtr<slang::IBlob>& OutDiagnostics
	) const -> Slang::Result
	{
		slang::IModule* Module = InSession->loadModule(InShaderFilePath, OutDiagnostics.writeRef());
		if (!Module)
		{
			return SLANG_FAIL;
		}

		std::vector<slang::IComponentType*> ComponentTypes;
		ComponentTypes.push_back(Module);

		std::vector<Slang::ComPtr<slang::IEntryPoint>> EntryPointObjects;
		for (const char8* Name : InEntryPoints)
		{
			Slang::ComPtr<slang::IEntryPoint> EntryPoint;
			SLANG_RETURN_ON_FAIL(Module->findEntryPointByName(Name, EntryPoint.writeRef()));
			EntryPointObjects.push_back(EntryPoint);
			ComponentTypes.push_back(EntryPoint.get());
		}

		SLANG_RETURN_ON_FAIL(InSession->createCompositeComponentType(
			ComponentTypes.data(),
			ComponentTypes.size(),
			OutComposedProgram.writeRef(),
			OutDiagnostics.writeRef()
		));

		return SLANG_OK;
	}

	auto FSlangShaderCompiler::InitGlobalSession() -> void
	{
		if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, GlobalSession.writeRef())))
		{
			throw std::runtime_error("slang_createGlobalSession failed");
		}
	}
} // namespace Durin
