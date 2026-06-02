#include "ShaderCompileUtilities.h"

#include "Hash/XxHash.h"
#include "Misc/FileFingerprintCache.h"

namespace Durin::ShaderCompileUtilities
{
	namespace
	{
		constexpr std::string_view GShaderSourceTreeSignatureVersion = "DurinShaderSourceTreeSignature_v2";
		constexpr std::string_view GShaderVariantKeyVersion = "DurinShaderVariantKey_v4";
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

	auto NormalizeMacros(const FShaderCompileOptions& Options, std::vector<FShaderMacroDefinition>& OutMacros, std::string& OutErrorMessage) -> bool
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

	auto BuildShaderMetaData(
		const std::vector<std::string>& InDependencyPaths,
		FFileFingerprintCache& FileFingerprintCache,
		FShaderMetaData& OutMetaData,
		std::string& OutErrorMessage
	) -> bool
	{
		OutMetaData = {};

		FXxHash128Builder TreeSignatureBuilder;
		UpdateHashStringField(TreeSignatureBuilder, GShaderSourceTreeSignatureVersion);

		for (const std::string& DependencyPath : InDependencyPaths)
		{
			FFileFingerprint Fingerprint;
			if (!FileFingerprintCache.TryGet(DependencyPath, Fingerprint, OutErrorMessage))
			{
				return false;
			}

			UpdateHashStringField(TreeSignatureBuilder, Fingerprint.NormalizedPath);
			TreeSignatureBuilder.UpdateValue(Fingerprint.FileSize);
			TreeSignatureBuilder.UpdateValue(Fingerprint.ContentHash);
		}

		OutMetaData.SourceTreeSignature = TreeSignatureBuilder.Finalize();
		return true;
	}

	auto BuildVariantKey(
		std::string_view VirtualShaderPath,
		const FShaderMetaData& MetaData,
		const std::vector<FShaderMacroDefinition>& Macros,
		FShaderVariantKey& OutVariantKey
	) -> void
	{
		FXxHash128Builder Builder;
		UpdateHashStringField(Builder, GShaderVariantKeyVersion);
		UpdateHashStringField(Builder, GSlangBackendName);
		UpdateHashStringField(Builder, GSlangTargetFormat);
		UpdateHashStringField(Builder, GSlangTargetProfile);
		UpdateHashStringField(Builder, VirtualShaderPath);
		Builder.UpdateValue(MetaData.SourceTreeSignature);

		const uint64 MacroCount = static_cast<uint64>(Macros.size());
		Builder.UpdateValue(MacroCount);
		for (const FShaderMacroDefinition& Macro : Macros)
		{
			UpdateHashStringField(Builder, Macro.Name);
			Builder.UpdateValue(Macro.HasValue());
			if (Macro.Value)
			{
				UpdateHashStringField(Builder, *Macro.Value);
			}
		}

		OutVariantKey.Value = Builder.Finalize();
		OutVariantKey.Hex = OutVariantKey.Value.ToString();
	}

	auto IsMetaDataCurrent(const FShaderMetaData& CurrentMetaData, const FShaderMetaData& CachedMetaData) -> bool
	{
		return CurrentMetaData.SourceTreeSignature == CachedMetaData.SourceTreeSignature;
	}
} // namespace Durin::ShaderCompileUtilities
