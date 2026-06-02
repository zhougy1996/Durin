#include "SlangShaderCompiler.h"

#include "Hash/XxHash.h"
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

		template <typename TBuilder>
		auto UpdateHashStringField(TBuilder& Builder, std::string_view Value) -> void
		{
			Builder.UpdateValue(static_cast<uint64>(Value.size()));
			Builder.Update(Value);
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
		UpdateHashStringField(TreeSignatureBuilder, GShaderVariantKeyVersion);

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
			Dependency.ContentHash = FXxHash64::HashBuffer(std::span<const uint8>(FileBytes));
			if (DependencyIndex == 0)
			{
				OutMetaData.MainSourceHash = Dependency.ContentHash;
			}

			UpdateHashStringField(TreeSignatureBuilder, Dependency.Path);
			TreeSignatureBuilder.UpdateValue(Dependency.FileSize);
			TreeSignatureBuilder.UpdateValue(Dependency.ContentHash);

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
		UpdateHashStringField(Builder, GShaderVariantKeyVersion);
		UpdateHashStringField(Builder, GSlangBackendName);
		UpdateHashStringField(Builder, GSlangTargetFormat);
		UpdateHashStringField(Builder, GSlangTargetProfile);
		UpdateHashStringField(Builder, MetaData.VirtualShaderPath);
		Builder.UpdateValue(MetaData.SourceTreeSignature);

		const uint64 MacroCount = static_cast<uint64>(Macros.size());
		Builder.UpdateValue(MacroCount);
		for (const FShaderMacroDefinition& Macro : Macros)
		{
			UpdateHashStringField(Builder, Macro.Name);
			UpdateHashStringField(Builder, Macro.Value);
			Builder.UpdateValue(Macro.bHasExplicitValue);
		}

		OutVariantKey.Value = Builder.Finalize();
		OutVariantKey.Hex = OutVariantKey.Value.ToString();
	}

	static auto IsMetaDataCurrent(const FShaderMetaData& CurrentMetaData, const FShaderMetaData& CachedMetaData) -> bool
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
			CompiledShader.Hash = FXxHash64::HashBuffer(*CompiledShader.Code);
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
		const bool bMetaDataCurrent = CacheStore.LoadMetaData(VirtualShaderPath, CachedMetaData) && IsMetaDataCurrent(CurrentMetaData, CachedMetaData);
		if (!bForceRecompile && bMetaDataCurrent && CacheStore.TryLoad(VirtualShaderPath, Options, VariantKey, Output))
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

		if (!CacheStore.Save(VirtualShaderPath, Options, VariantKey, Output))
		{
			DURIN_WARN("Shader compiled successfully, but cache write failed for {}", VirtualShaderPath);
		}
		if (!CacheStore.SaveMetaData(CurrentMetaData))
		{
			DURIN_WARN("Shader compiled successfully, but meta write failed for {}", VirtualShaderPath);
		}

		Output.bSucceeded = true;
		return Output;
	}

	auto FSlangShaderCompiler::InitGlobalSession() -> void
	{
		if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, GlobalSession.writeRef())))
		{
			throw std::runtime_error("slang_createGlobalSession failed");
		}
	}
} // namespace Durin
