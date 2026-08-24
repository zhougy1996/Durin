#include "SlangShaderDependencyResolver.h"

#include "ShaderCompileUtilities.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view GSlangTargetProfile = "spirv_1_5";

		auto NormalizePath(const std::filesystem::path& InPath) -> std::string
		{
			std::error_code ErrorCode;
			const std::filesystem::path CanonicalPath = std::filesystem::weakly_canonical(InPath, ErrorCode);
			if (!ErrorCode)
			{
				return CanonicalPath.generic_string();
			}
			return InPath.lexically_normal().generic_string();
		}
	}

	FSlangShaderDependencyResolver::FSlangShaderDependencyResolver()
	{
		InitGlobalSession();
	}

	FSlangShaderDependencyResolver::~FSlangShaderDependencyResolver()
	{
		GlobalSession.setNull();
	}

	auto FSlangShaderDependencyResolver::CreateSession(
		const FShaderCompileOptions& Options,
		Slang::ComPtr<slang::ISession>& OutSession,
		std::string& OutErrorMessage, std::string_view SearchPath) const -> bool
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
			MacroDesc.value = Macro.Value ? Macro.Value->c_str() : nullptr;
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
		const std::string SearchPathStorage(SearchPath);
		const char* SearchPathPointer = SearchPathStorage.c_str();
		if (!SearchPath.empty())
		{
			SessionDesc.searchPaths = &SearchPathPointer;
			SessionDesc.searchPathCount = 1;
		}

		if (SLANG_FAILED(GlobalSession->createSession(SessionDesc, OutSession.writeRef())))
		{
			OutErrorMessage = "createSession failed";
			return false;
		}

		return true;
	}

	auto FSlangShaderDependencyResolver::Resolve(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options, std::vector<std::string>& OutDependencyPaths, std::string& OutDiagnostics) const -> bool
	{
		Slang::ComPtr<slang::ISession> Session;
		if (!CreateSession(Options, Session, OutDiagnostics))
		{
			return false;
		}

		const std::string SourceFilePath(ShaderSourceFilePath);
		Slang::ComPtr<slang::IBlob> DiagnosticsBlob;
		slang::IModule* Module = Session->loadModule(SourceFilePath.data(), DiagnosticsBlob.writeRef());
		if (!Module)
		{
			if (DiagnosticsBlob)
			{
				OutDiagnostics = static_cast<const char*>(DiagnosticsBlob->getBufferPointer());
			}
			return false;
		}

		OutDependencyPaths.clear();
		OutDependencyPaths.push_back(NormalizePath(std::filesystem::path(SourceFilePath)));

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
		return true;
	}

	auto FSlangShaderDependencyResolver::ResolveSource(
		std::string_view ModuleName, std::string_view SourcePathHint,
		std::string_view Source, const FShaderCompileOptions& Options,
		std::vector<std::string>& OutDependencyPaths,
		std::string& OutDiagnostics) const -> bool
	{
		Slang::ComPtr<slang::ISession> Session;
		if (!CreateSession(Options, Session, OutDiagnostics,
			std::filesystem::path(SourcePathHint).parent_path().generic_string()))
			return false;
		const std::string Name(ModuleName);
		const std::string Path(SourcePathHint);
		const std::string Text(Source);
		Slang::ComPtr<slang::IBlob> Diagnostics;
		slang::IModule* Module = Session->loadModuleFromSourceString(
			Name.c_str(), Path.c_str(), Text.c_str(), Diagnostics.writeRef());
		if (!Module)
		{
			OutDiagnostics = Diagnostics
				? static_cast<const char*>(Diagnostics->getBufferPointer())
				: "Failed to resolve generated shader module";
			return false;
		}
		OutDependencyPaths.clear();
		const std::string NormalizedSourcePath = NormalizePath(Path);
		for (SlangInt Index = 0;
			Index < Module->getDependencyFileCount(); ++Index)
		{
			const char* Dependency = Module->getDependencyFilePath(Index);
			if (Dependency && Dependency[0] != '\0')
			{
				std::string NormalizedDependency = NormalizePath(Dependency);
				if (NormalizedDependency != NormalizedSourcePath)
					OutDependencyPaths.push_back(std::move(NormalizedDependency));
			}
		}
		std::ranges::sort(OutDependencyPaths);
		OutDependencyPaths.erase(
			std::ranges::unique(OutDependencyPaths).begin(),
			OutDependencyPaths.end());
		return true;
	}

	auto FSlangShaderDependencyResolver::InitGlobalSession() -> void
	{
		if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, GlobalSession.writeRef())))
		{
			throw std::runtime_error("slang_createGlobalSession failed");
		}
	}
}
