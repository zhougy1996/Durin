#include "SlangSessionEnvironment.h"

namespace Durin
{
	auto FSlangSessionEnvironment::NormalizeMacros(
		const FShaderCompileOptions& Options,
		std::vector<FShaderMacroDefinition>& OutMacros,
		std::string& OutErrorMessage
	) -> bool
	{
		OutMacros = Options.Macros;
		std::ranges::sort(OutMacros, [](const FShaderMacroDefinition& A, const FShaderMacroDefinition& B) {
			if (A.Name != B.Name)
			{
				return A.Name < B.Name;
			}
			if (A.HasValue() != B.HasValue())
			{
				return !A.HasValue();
			}
			return A.Value < B.Value;
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

	auto FSlangSessionEnvironment::CreateSession(
		slang::IGlobalSession& GlobalSession,
		const FShaderCompileOptions& Options,
		Slang::ComPtr<slang::ISession>& OutSession,
		std::string& OutErrorMessage,
		std::string_view SearchPath
	) -> bool
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
			MacroDesc.value = Macro.Value ? Macro.Value->c_str() : nullptr;
			SlangMacros.push_back(MacroDesc);
		}

		slang::TargetDesc TargetDesc = {};
		TargetDesc.format = TargetFormat;
		TargetDesc.profile = GlobalSession.findProfile(TargetProfileName.data());

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

		if (SLANG_FAILED(GlobalSession.createSession(SessionDesc, OutSession.writeRef())))
		{
			OutErrorMessage = "createSession failed";
			return false;
		}

		return true;
	}
} // namespace Durin
