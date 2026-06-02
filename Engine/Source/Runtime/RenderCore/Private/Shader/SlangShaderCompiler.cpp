#include "SlangShaderCompiler.h"

#include "ShaderCompileUtilities.h"

#include "Hash/XxHash.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view GSlangTargetProfile = "spirv_1_5";
	}

	FSlangShaderCompiler::FSlangShaderCompiler()
	{
		InitGlobalSession();
	}

	FSlangShaderCompiler::~FSlangShaderCompiler()
	{
		FileFingerprintCache.Clear();
		GlobalSession.setNull();
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

	auto FSlangShaderCompiler::CreateSession(const FShaderCompileOptions& Options, Slang::ComPtr<slang::ISession>& OutSession, std::string& OutErrorMessage) const -> bool
	{
		std::vector<FShaderMacroDefinition> NormalizedMacros;
		if (!ShaderCompileUtilities::NormalizeMacros(Options, NormalizedMacros, OutErrorMessage))
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
		if (!ShaderCompileUtilities::NormalizeMacros(Options, NormalizedMacros, Output.ErrorMessage))
		{
			return Output;
		}

		const bool bForceRecompile = Options.bForceRecompile || Settings.bForceRecompile;
		const std::string SourceFilePath(ShaderSourceFilePath);
		const std::string VirtualShaderPath = Options.VirtualShaderPath;
		const bool bUseDiskCache = !VirtualShaderPath.empty();

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
		if (!ShaderCompileUtilities::BuildShaderMetaData(DependencyPaths, FileFingerprintCache, CurrentMetaData, Output.ErrorMessage))
		{
			return Output;
		}

		FShaderVariantKey VariantKey;
		if (bUseDiskCache)
		{
			ShaderCompileUtilities::BuildVariantKey(VirtualShaderPath, CurrentMetaData, NormalizedMacros, VariantKey);

			FShaderMetaData CachedMetaData;
			const bool bMetaDataCurrent = CacheStore.LoadMetaData(VirtualShaderPath, CachedMetaData)
				&& ShaderCompileUtilities::IsMetaDataCurrent(CurrentMetaData, CachedMetaData);
			if (!bForceRecompile && bMetaDataCurrent && CacheStore.TryLoad(VirtualShaderPath, Options, VariantKey, Output))
			{
				return Output;
			}
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

		if (bUseDiskCache && !CacheStore.Save(VirtualShaderPath, Options, VariantKey, Output))
		{
			DURIN_WARN("Shader compiled successfully, but cache write failed for {}", VirtualShaderPath);
		}
		if (bUseDiskCache && !CacheStore.SaveMetaData(VirtualShaderPath, CurrentMetaData))
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
