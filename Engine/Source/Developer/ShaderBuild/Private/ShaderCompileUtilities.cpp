#include "ShaderCompileUtilities.h"
#include "ShaderCaptureLimits.h"
#include "Hash/CanonicalHash.h"

#include "Hash/XxHash.h"
#include "Misc/FileFingerprintCache.h"
#include "Misc/FileHelper.h"
#include "ShaderBuild/ShaderPaths.h"
#include "SlangSessionEnvironment.h"

namespace Durin::ShaderCompileUtilities
{
	namespace
	{
		constexpr std::string_view GShaderSourceTreeSignatureVersion = "DurinShaderPortableSourceTree_v1";
		constexpr std::string_view GShaderVariantKeyVersion = "DurinShaderVariantKey_v6";
		constexpr std::string_view GShaderDependencyKeyVersion = "DurinShaderDependencyKey_v1";
	}

	// Resolve only the dependency generation used to construct the variant key.
	auto CaptureSourceArtifacts(const FShaderMetaData& MetaData) -> std::expected<std::shared_ptr<const FShaderSourceArtifacts>, FShaderError>
	{
		if (MetaData.Dependencies.size() != MetaData.PortableDependencies.size()
			|| MetaData.Dependencies.size() > ShaderCaptureLimits::MaximumFiles)
			return std::unexpected(FShaderError{.Code = EShaderError::CaptureInputInvalid});
		std::map<std::string, FByteBuffer> Files;
		uint64 TotalBytes = 0;
		for (size_t Index = 0; Index < MetaData.Dependencies.size(); ++Index)
		{
			const auto& Expected = MetaData.Dependencies[Index];
			const auto& Portable = MetaData.PortableDependencies[Index];
			if (Expected.FileSize > ShaderCaptureLimits::MaximumFileBytes
				|| Expected.FileSize > ShaderCaptureLimits::MaximumTotalBytes - TotalBytes)
				return std::unexpected(FShaderError{.Code = EShaderError::CaptureInputInvalid});
			TotalBytes += Expected.FileSize;
			auto Loaded = FFileHelper::LoadFileToArray(Expected.NormalizedPath,
				{.MaximumBytes = ShaderCaptureLimits::MaximumFileBytes, .ExpectedBytes = Expected.FileSize});
			if (!Loaded)
			{
				if (Loaded.error().Operation == EFileOperation::QuerySize
					&& Loaded.error().NativeError == std::errc::message_size)
					return std::unexpected(FShaderError{.Code = EShaderError::DependencyContentConflict, .ActualIdentity = Portable.VirtualPath});
				return std::unexpected(FShaderError{.Code = EShaderError::FileReadFailure, .FileError = std::move(Loaded.error())});
			}
			auto& Bytes = *Loaded;
			if (FXxHash64::HashBuffer(Bytes) != Expected.ContentHash || Expected.ContentHash != Portable.ContentHash)
				return std::unexpected(FShaderError{.Code = EShaderError::DependencyContentConflict, .ActualIdentity = Portable.VirtualPath});
			// Portable module identities omit .slang; the captured filesystem needs the filename.
			const auto Filename = Portable.VirtualPath + (Expected.NormalizedPath.ends_with(".slang") ? ".slang" : "");
			Files.emplace(Filename, std::move(Bytes));
		}
		std::vector<std::string> Roots;
		for (const auto& Mount : FShaderPaths::GetRegisteredMountPoints()) Roots.push_back(Mount.VirtualRoot);
		return std::make_shared<const FShaderSourceArtifacts>(std::move(Files), std::move(Roots));
	}

	auto NormalizeMacros(const FShaderCompileOptions& Options, std::vector<FShaderMacroDefinition>& OutMacros) -> FShaderOperationResult
	{
		return FSlangSessionEnvironment::NormalizeMacros(Options, OutMacros);
	}

	auto BuildShaderMetaData(
		const std::vector<std::string>& InDependencyPaths,
		FFileFingerprintCache& FileFingerprintCache,
		FShaderMetaData& OutMetaData) -> FShaderOperationResult
	{
		OutMetaData = {};

		for (const std::string& DependencyPath : InDependencyPaths)
		{
			auto Fingerprint = FileFingerprintCache.Get(DependencyPath);
			if (!Fingerprint)
			{
				return std::unexpected(FShaderError{.Code = EShaderError::FileReadFailure, .FileError = std::move(Fingerprint.error())});
			}

			std::string VirtualPath;
			if (!FShaderPaths::TryMakeVirtualSourcePath(
				Fingerprint->NormalizedPath, VirtualPath))
			{
				return std::unexpected(FShaderError{.Code = EShaderError::DependencyIdentityMissing, .ActualIdentity = Fingerprint->NormalizedPath});
			}
			OutMetaData.Dependencies.push_back(std::move(*Fingerprint));
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
					return std::unexpected(FShaderError{.Code = EShaderError::DependencyContentConflict, .ActualIdentity = Dependency.VirtualPath});
				}
				continue;
			}
			Sorted.Dependencies.push_back(
				std::move(OutMetaData.Dependencies[Index]));
			Sorted.PortableDependencies.push_back(
				std::move(OutMetaData.PortableDependencies[Index]));
		}

		FXxHash128Builder TreeSignatureBuilder;
		UpdateCanonicalHashString(TreeSignatureBuilder,
			GShaderSourceTreeSignatureVersion);
		UpdateCanonicalHash(TreeSignatureBuilder,
			static_cast<uint64>(Sorted.PortableDependencies.size()));
		for (const FShaderPortableDependency& Dependency
			: Sorted.PortableDependencies)
		{
			UpdateCanonicalHashString(TreeSignatureBuilder, Dependency.VirtualPath);
			UpdateCanonicalHash(TreeSignatureBuilder, Dependency.ContentHash);
		}

		Sorted.SourceTreeSignature = TreeSignatureBuilder.Finalize();
		OutMetaData = std::move(Sorted);
		return {};
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
		UpdateCanonicalHashString(Builder, GShaderVariantKeyVersion);
		UpdateCanonicalHashString(Builder, FSlangSessionEnvironment::BackendName);
		UpdateCanonicalHashString(Builder, FSlangSessionEnvironment::TargetFormatName);
		UpdateCanonicalHashString(Builder, FSlangSessionEnvironment::TargetProfileName);
		UpdateCanonicalHashString(Builder, CompilerEnvironment);
		UpdateCanonicalHashString(Builder, VirtualShaderPath);
		UpdateCanonicalHash(Builder, MetaData.SourceTreeSignature);

		const uint64 MacroCount = static_cast<uint64>(Macros.size());
		UpdateCanonicalHash(Builder, MacroCount);
		for (const FShaderMacroDefinition& Macro : Macros)
		{
			UpdateCanonicalHashString(Builder, Macro.Name);
			UpdateCanonicalHash(Builder, Macro.HasValue());
			if (Macro.Value)
			{
				UpdateCanonicalHashString(Builder, *Macro.Value);
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
		UpdateCanonicalHashString(Builder, GShaderDependencyKeyVersion);
		UpdateCanonicalHashString(Builder, VirtualShaderPath);
		UpdateCanonicalHashString(Builder, CompilerEnvironment);
		UpdateCanonicalHash(Builder, static_cast<uint64>(Macros.size()));
		for (const FShaderMacroDefinition& Macro : Macros)
		{
			UpdateCanonicalHashString(Builder, Macro.Name);
			UpdateCanonicalHash(Builder, Macro.HasValue());
			if (Macro.Value)
			{
				UpdateCanonicalHashString(Builder, *Macro.Value);
			}
		}
		OutDependencyKey.Value = Builder.Finalize();
		OutDependencyKey.Hex = OutDependencyKey.Value.ToString();
	}

	auto TryReuseMetaData(
		const FShaderMetaData& CachedMetaData,
		FFileFingerprintCache& FileFingerprintCache
	) -> FMetaDataReuseResult
	{
		if (CachedMetaData.SourceTreeSignature.IsZero()
			|| CachedMetaData.Dependencies.size()
				!= CachedMetaData.PortableDependencies.size())
		{
			return {.Status = EMetaDataReuseStatus::Stale};
		}

		for (const FFileFingerprint& Fingerprint : CachedMetaData.Dependencies)
		{
			FFileFingerprintReuseResult ReuseResult =
				FileFingerprintCache.TryReuse(Fingerprint);
			if (!ReuseResult)
			{
				return {.Status = EMetaDataReuseStatus::Failed,
					.Error = {.Code = EShaderError::FileReadFailure, .FileError = std::move(ReuseResult.error())}};
			}
			if (*ReuseResult == EFileFingerprintReuseStatus::Stale)
			{
				return {.Status = EMetaDataReuseStatus::Stale};
			}
		}

		FXxHash128Builder SignatureBuilder;
		UpdateCanonicalHashString(SignatureBuilder, GShaderSourceTreeSignatureVersion);
		UpdateCanonicalHash(SignatureBuilder, static_cast<uint64>(
			CachedMetaData.PortableDependencies.size()));
		for (size_t Index = 0;
			Index < CachedMetaData.PortableDependencies.size(); ++Index)
		{
			const FShaderPortableDependency& Dependency =
				CachedMetaData.PortableDependencies[Index];
			if (Dependency.ContentHash
				!= CachedMetaData.Dependencies[Index].ContentHash)
				return {.Status = EMetaDataReuseStatus::Stale};
			UpdateCanonicalHashString(SignatureBuilder, Dependency.VirtualPath);
			UpdateCanonicalHash(SignatureBuilder, Dependency.ContentHash);
		}
		if (SignatureBuilder.Finalize() != CachedMetaData.SourceTreeSignature)
		{
			return {.Status = EMetaDataReuseStatus::Stale};
		}

		return {.Status = EMetaDataReuseStatus::Current};
	}
} // namespace Durin::ShaderCompileUtilities
