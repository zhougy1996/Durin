#include "ShaderCompileUtilities.h"

#include "Hash/XxHash.h"
#include "Misc/FileFingerprintCache.h"
#include "ShaderBuild/ShaderPaths.h"

namespace Durin::ShaderCompileUtilities
{
	namespace
	{
		constexpr std::string_view GShaderSourceTreeSignatureVersion = "DurinShaderPortableSourceTree_v1";
		constexpr std::string_view GShaderVariantKeyVersion = "DurinShaderVariantKey_v6";
		constexpr std::string_view GShaderDependencyKeyVersion = "DurinShaderDependencyKey_v1";
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

		for (const std::string& DependencyPath : InDependencyPaths)
		{
			FFileFingerprint Fingerprint;
			if (!FileFingerprintCache.TryGet(DependencyPath, Fingerprint, OutErrorMessage))
			{
				return false;
			}

			std::string VirtualPath;
			if (!FShaderPaths::TryMakeVirtualSourcePath(
				Fingerprint.NormalizedPath, VirtualPath))
			{
				OutErrorMessage = std::format(
					"Shader dependency has no registered virtual identity: {}",
					Fingerprint.NormalizedPath);
				return false;
			}
			OutMetaData.Dependencies.push_back(std::move(Fingerprint));
			OutMetaData.PortableDependencies.push_back({
				.VirtualPath = std::move(VirtualPath),
				.ContentHash = OutMetaData.Dependencies.back().ContentHash});
		}
		std::vector<size_t> Order(OutMetaData.Dependencies.size());
		std::iota(Order.begin(), Order.end(), size_t{0});
		std::ranges::sort(Order, [&](size_t A, size_t B) {
			return OutMetaData.PortableDependencies[A].VirtualPath
				< OutMetaData.PortableDependencies[B].VirtualPath;
		});
		FShaderMetaData Sorted;
		Sorted.Dependencies.reserve(Order.size());
		Sorted.PortableDependencies.reserve(Order.size());
		for (size_t Index : Order)
		{
			const FShaderPortableDependency& Dependency =
				OutMetaData.PortableDependencies[Index];
			if (!Sorted.PortableDependencies.empty()
				&& Sorted.PortableDependencies.back().VirtualPath
					== Dependency.VirtualPath)
			{
				if (Sorted.PortableDependencies.back().ContentHash
					!= Dependency.ContentHash)
				{
					OutErrorMessage = std::format(
						"Shader virtual dependency resolves to conflicting content: {}",
						Dependency.VirtualPath);
					return false;
				}
				continue;
			}
			Sorted.Dependencies.push_back(
				std::move(OutMetaData.Dependencies[Index]));
			Sorted.PortableDependencies.push_back(
				std::move(OutMetaData.PortableDependencies[Index]));
		}

		FXxHash128Builder TreeSignatureBuilder;
		UpdateHashStringField(TreeSignatureBuilder,
			GShaderSourceTreeSignatureVersion);
		TreeSignatureBuilder.UpdateValue(
			static_cast<uint64>(Sorted.PortableDependencies.size()));
		for (const FShaderPortableDependency& Dependency
			: Sorted.PortableDependencies)
		{
			UpdateHashStringField(TreeSignatureBuilder, Dependency.VirtualPath);
			TreeSignatureBuilder.UpdateValue(Dependency.ContentHash);
		}

		Sorted.SourceTreeSignature = TreeSignatureBuilder.Finalize();
		OutMetaData = std::move(Sorted);
		return true;
	}

	auto BuildVariantKey(
		std::string_view VirtualShaderPath,
		const FShaderMetaData& MetaData,
		const std::vector<FShaderMacroDefinition>& Macros,
		std::string_view CompilerEnvironment,
		FShaderVariantKey& OutVariantKey
	) -> void
	{
		FXxHash128Builder Builder;
		UpdateHashStringField(Builder, GShaderVariantKeyVersion);
		UpdateHashStringField(Builder, GSlangBackendName);
		UpdateHashStringField(Builder, GSlangTargetFormat);
		UpdateHashStringField(Builder, GSlangTargetProfile);
		UpdateHashStringField(Builder, CompilerEnvironment);
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

	auto BuildDependencyKey(
		std::string_view VirtualShaderPath,
		const std::vector<FShaderMacroDefinition>& Macros,
		std::string_view CompilerEnvironment,
		FShaderDependencyKey& OutDependencyKey
	) -> void
	{
		FXxHash128Builder Builder;
		UpdateHashStringField(Builder, GShaderDependencyKeyVersion);
		UpdateHashStringField(Builder, VirtualShaderPath);
		UpdateHashStringField(Builder, CompilerEnvironment);
		Builder.UpdateValue(static_cast<uint64>(Macros.size()));
		for (const FShaderMacroDefinition& Macro : Macros)
		{
			UpdateHashStringField(Builder, Macro.Name);
			Builder.UpdateValue(Macro.HasValue());
			if (Macro.Value)
			{
				UpdateHashStringField(Builder, *Macro.Value);
			}
		}
		OutDependencyKey.Value = Builder.Finalize();
		OutDependencyKey.Hex = OutDependencyKey.Value.ToString();
	}

	auto TryReuseMetaData(
		const FShaderMetaData& CachedMetaData,
		FFileFingerprintCache& FileFingerprintCache,
		bool& bOutCurrent,
		std::string& OutErrorMessage
	) -> bool
	{
		bOutCurrent = false;
		if (CachedMetaData.SourceTreeSignature.IsZero()
			|| CachedMetaData.Dependencies.size()
				!= CachedMetaData.PortableDependencies.size())
		{
			return true;
		}

		for (const FFileFingerprint& Fingerprint : CachedMetaData.Dependencies)
		{
			bool bFingerprintCurrent = false;
			if (!FileFingerprintCache.TryReuse(Fingerprint, bFingerprintCurrent, OutErrorMessage))
			{
				return false;
			}
			if (!bFingerprintCurrent)
			{
				return true;
			}
		}

		FXxHash128Builder SignatureBuilder;
		UpdateHashStringField(SignatureBuilder, GShaderSourceTreeSignatureVersion);
		SignatureBuilder.UpdateValue(static_cast<uint64>(
			CachedMetaData.PortableDependencies.size()));
		for (size_t Index = 0;
			Index < CachedMetaData.PortableDependencies.size(); ++Index)
		{
			const FShaderPortableDependency& Dependency =
				CachedMetaData.PortableDependencies[Index];
			if (Dependency.ContentHash
				!= CachedMetaData.Dependencies[Index].ContentHash) return true;
			UpdateHashStringField(SignatureBuilder, Dependency.VirtualPath);
			SignatureBuilder.UpdateValue(Dependency.ContentHash);
		}
		if (SignatureBuilder.Finalize() != CachedMetaData.SourceTreeSignature)
		{
			return true;
		}

		bOutCurrent = true;
		return true;
	}
} // namespace Durin::ShaderCompileUtilities
