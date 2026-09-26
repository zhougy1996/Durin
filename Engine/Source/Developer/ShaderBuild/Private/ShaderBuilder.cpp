#include "ShaderBuilder.h"
#include "Hash/CanonicalHash.h"

#include "ShaderCompileUtilities.h"
#include "ShaderDerivedData.h"
#include "DerivedDataBuild.h"
#include "ShaderDependencyManifestStore.h"
#include "SlangShaderCompiler.h"
#include "SlangShaderDependencyResolver.h"

#include "Misc/FileFingerprintCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ShaderBuild/ShaderPaths.h"

#include <tuple>

namespace Durin
{
	namespace
	{
		auto BuildRequestKey(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const std::vector<FShaderMacroDefinition>& Macros) -> std::string
		{
			FXxHash128Builder Builder;
			UpdateCanonicalHashString(Builder, "DurinShaderCompileRequest_v1");
			UpdateCanonicalHashString(Builder, VirtualShaderPath);
			UpdateCanonicalHashString(Builder, Options.CompilerEnvironment);
			UpdateCanonicalHash(Builder, Options.bForceRecompile);
			UpdateCanonicalHash(Builder, static_cast<uint64>(Options.EntryPoints.size()));
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				UpdateCanonicalHashString(Builder, Options.EntryPoints[Index] ? std::string_view(Options.EntryPoints[Index]) : std::string_view{});
				if (Index < Options.Frequencies.size()) UpdateCanonicalHash(Builder, Options.Frequencies[Index]);
			}
			UpdateCanonicalHash(Builder, static_cast<uint64>(Macros.size()));
			for (const FShaderMacroDefinition& Macro : Macros)
			{
				UpdateCanonicalHashString(Builder, Macro.Name);
				UpdateCanonicalHash(Builder, Macro.HasValue());
				if (Macro.Value) UpdateCanonicalHashString(Builder, *Macro.Value);
			}
			return Builder.Finalize().ToString();
		}

		auto BuildOutputKey(const FShaderVariantKey& VariantKey, const FShaderCompileOptions& Options) -> std::string
		{
			return std::string(
				ShaderDerivedData::BuildKey(VariantKey, Options).ToString());
		}

		auto HasValidUniqueEntryPoints(const FShaderCompileOptions& Options) -> bool
		{
			if (Options.EntryPoints.empty()
				|| Options.EntryPoints.size() != Options.Frequencies.size()
				|| Options.EntryPoints.size()
					> ShaderDerivedData::MaximumEntryPoints) return false;
			std::set<std::pair<std::string_view, uint32>> Entries;
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				const std::string_view Entry = Options.EntryPoints[Index]
					? std::string_view(Options.EntryPoints[Index]) : std::string_view{};
				const uint32 Frequency =
					static_cast<uint32>(Options.Frequencies[Index]);
				if (Entry.empty()
					|| Frequency > static_cast<uint32>(EShaderFrequency::RayMiss)
					|| !Entries.emplace(Entry, Frequency).second) return false;
			}
			return true;
		}

		using FArtifactResult = std::expected<std::shared_ptr<const FShaderSourceArtifacts>, FShaderError>;

		auto CapturedMetaData(std::vector<std::string> Paths, const FShaderSourceArtifacts& Artifacts)
			-> std::expected<FShaderMetaData, FShaderError>
		{
			std::ranges::sort(Paths);
			Paths.erase(std::unique(Paths.begin(), Paths.end()), Paths.end());
			FShaderMetaData Result;
			for (const auto& Path : Paths)
			{
				const auto Found = Artifacts.GetFiles().find(Path);
				if (Found == Artifacts.GetFiles().end())
					return std::unexpected(FShaderError{.Code = EShaderError::DependencyNotCaptured, .ActualIdentity = Path});
				const auto ContentHash = FXxHash64::HashBuffer(Found->second);
				auto Identity = Path.ends_with(".slang") ? Path.substr(0, Path.size() - 6) : Path;
				std::string MountedIdentity;
				if (FShaderPaths::TryMakeVirtualSourcePath(Path, MountedIdentity)) Identity = std::move(MountedIdentity);
				Result.PortableDependencies.push_back({Identity, ContentHash});
			}
			std::ranges::sort(Result.PortableDependencies, {}, &FShaderPortableDependency::VirtualPath);
			for (size_t Index = 1; Index < Result.PortableDependencies.size(); ++Index)
				if (Result.PortableDependencies[Index - 1].VirtualPath == Result.PortableDependencies[Index].VirtualPath
					&& Result.PortableDependencies[Index - 1].ContentHash != Result.PortableDependencies[Index].ContentHash)
					return std::unexpected(FShaderError{.Code = EShaderError::DependencyContentConflict,
						.ActualIdentity = Result.PortableDependencies[Index].VirtualPath});
			Result.PortableDependencies.erase(std::unique(Result.PortableDependencies.begin(), Result.PortableDependencies.end()),
				Result.PortableDependencies.end());
			FXxHash128Builder Hash;
			UpdateCanonicalHashString(Hash, "DurinShaderPortableSourceTree_v1");
			UpdateCanonicalHash(Hash, uint64(Result.PortableDependencies.size()));
			for (const auto& Dependency : Result.PortableDependencies)
			{
				UpdateCanonicalHashString(Hash, Dependency.VirtualPath);
				UpdateCanonicalHash(Hash, Dependency.ContentHash);
			}
			Result.SourceTreeSignature = Hash.Finalize();
			return Result;
		}

		struct FShaderBuildAdapter
		{
			using FProduct = FShaderCompilerOutput;
			using FError = FShaderError;
			const FShaderCompileOptions& Options;
			const DerivedData::FBuildDefinition& Definition;
			std::array<DerivedData::FBuildInputReference, 1> Inputs;
			std::function<FArtifactResult()> ResolveArtifacts;
			std::function<FProduct(const FShaderCompileOptions&)> Compile;
			auto GetFunction() const -> DerivedData::FBuildFunctionDescriptor
			{
				auto Function = Definition.GetFunction();
				Function.Version = ShaderDerivedData::BuilderVersion;
				return Function;
			}
			auto GetInputs() const -> std::span<const DerivedData::FBuildInputReference> { return Inputs; }
			auto MakeError(DerivedData::EBuildFailure Code) const -> FError
			{
				return {.Code = Code == DerivedData::EBuildFailure::Cancelled ? EShaderError::Cancelled : EShaderError::InvalidCompileRequest};
			}
			auto IsCancelled(const FError& Error) const -> bool { return Error.Code == EShaderError::Cancelled; }
			auto ValidateBindings(const DerivedData::FBuildDefinition&) const -> FShaderOperationResult { return {}; }
			auto Resolve() const -> std::expected<FShaderCompileOptions, FError>
			{
				auto Artifacts = ResolveArtifacts();
				if (!Artifacts) return std::unexpected(std::move(Artifacts.error()));
				if (!*Artifacts) return std::unexpected(FError{.Code = EShaderError::DependencyNotCaptured});
				auto Prepared = Options;
				Prepared.SourceArtifacts = std::move(*Artifacts);
				return Prepared;
			}
			auto Build(FShaderCompileOptions& Prepared) const -> std::expected<FProduct, FError>
			{
				auto Product = Compile(Prepared);
				if (!Product) return std::unexpected(std::move(Product.Error));
				return Product;
			}
			auto Validate(const FProduct& Product) const -> FShaderOperationResult
			{
				if (!Product || Product.CompiledShaders.size() != Options.EntryPoints.size())
					return std::unexpected(FError{.Code = EShaderError::PayloadOutputInvalid});
				for (size_t Index = 0; Index < Product.CompiledShaders.size(); ++Index)
				{
					const auto& Shader = Product.CompiledShaders[Index];
					if (!Shader.Code || Shader.Code->empty() || Shader.SourceEntryPoint != Options.EntryPoints[Index]
						|| Shader.Frequency != Options.Frequencies[Index] || FXxHash128::HashBuffer(*Shader.Code) != Shader.Hash)
						return std::unexpected(FError{.Code = EShaderError::PayloadOutputInvalid, .Index = Index});
				}
				return {};
			}
			auto Decode(const FSharedByteBuffer& Bytes) const -> std::expected<FProduct, FError>
			{
				FProduct Product;
				auto Result = ShaderDerivedData::Decode(Bytes.GetBytes(), Options, Product);
				if (!Result) return std::unexpected(std::move(Result.error()));
				return Product;
			}
			auto Encode(FProduct& Product) const -> std::expected<FByteBuffer, FError>
			{
				FByteBuffer Bytes;
				auto Result = ShaderDerivedData::Encode(Options, Product, Bytes);
				if (!Result) return std::unexpected(std::move(Result.error()));
				return Bytes;
			}
		};

	}

	FShaderBuilder::FShaderBuilder(std::function<void(std::string_view)> InBeforeGeneratedCompile)
		: CompilerEnvironmentIdentity(Compiler.GetEnvironmentIdentity())
		, BeforeGeneratedCompile(std::move(InBeforeGeneratedCompile))
	{
	}

	FShaderBuilder::~FShaderBuilder()
	{
		FileFingerprintCache.Clear();
	}

	auto FShaderBuilder::GetCompilerEnvironmentIdentity() const -> const std::string&
	{
		return CompilerEnvironmentIdentity;
	}

	auto FShaderBuilder::BuildSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult
	{
		OutDependencies.clear();

		if (VirtualShaderPath.empty())
		{
			return std::unexpected(FShaderError{.Code = EShaderError::MissingVirtualPath});
		}
		if (Options.SourceArtifacts)
		{
			std::vector<std::string> Paths;
			if (auto Result = DependencyResolver.Resolve(Options.SourceArtifacts->GetFiles().contains(std::string(VirtualShaderPath))
				? std::string(VirtualShaderPath) : std::string(VirtualShaderPath) + ".slang", Options, Paths); !Result)
				return Result;
			for (const auto& Path : Paths)
			{
				const auto Found = Options.SourceArtifacts->GetFiles().find(Path);
				if (Found == Options.SourceArtifacts->GetFiles().end())
				{
					OutDependencies.clear();
					return std::unexpected(FShaderError{.Code = EShaderError::DependencyNotCaptured, .ActualIdentity = Path});
				}
				OutDependencies.push_back({Path, FXxHash128::HashBuffer(Found->second)});
			}
			return {};
		}
		FShaderCompileOptions EffectiveOptions = Options;
		EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);
		EffectiveOptions.CompilerEnvironment = CompilerEnvironmentIdentity;
		std::vector<std::string> PhysicalDependencies;
		if (auto Result = DependencyResolver.Resolve(FShaderPaths::SourcePath(VirtualShaderPath), EffectiveOptions, PhysicalDependencies); !Result) return Result;

		OutDependencies.reserve(PhysicalDependencies.size());
		for (const std::string& PhysicalPath : PhysicalDependencies)
		{
			std::string VirtualPath;
			if (!FShaderPaths::TryMakeVirtualSourcePath(
				PhysicalPath, VirtualPath))
			{
				OutDependencies.clear();
				return std::unexpected(FShaderError{.Code = EShaderError::DependencyIdentityMissing, .ActualIdentity = PhysicalPath});
			}
			auto ContentHash = FFileHelper::HashFileXx128(PhysicalPath);
			if (!ContentHash)
			{
				OutDependencies.clear();
				return std::unexpected(FShaderError{.Code = EShaderError::FileReadFailure, .FileError = std::move(ContentHash.error())});
			}
			OutDependencies.push_back({
				.VirtualPath = std::move(VirtualPath),
				.ContentHash = *ContentHash});
		}
		std::ranges::sort(OutDependencies, [](const auto& A, const auto& B) {
			return std::tie(A.VirtualPath, A.ContentHash.HashHigh,
				A.ContentHash.HashLow)
				< std::tie(B.VirtualPath, B.ContentHash.HashHigh,
					B.ContentHash.HashLow);
		});
		OutDependencies.erase(
			std::unique(OutDependencies.begin(), OutDependencies.end()),
			OutDependencies.end());
		return {};
	}

	auto FShaderBuilder::BuildSourceTreeFingerprint(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult
	{
		OutFingerprint = {};

		if (VirtualShaderPath.empty())
		{
			return std::unexpected(FShaderError{.Code = EShaderError::MissingVirtualPath});
		}
		if (Options.SourceArtifacts)
		{
			std::vector<FShaderSourceDependencyFingerprint> Dependencies;
			if (auto Result = BuildSourceDependencyManifest(VirtualShaderPath, Options, Dependencies); !Result)
				return Result;
			FXxHash128Builder Hash;
			Hash.Update("DurinCapturedShaderSources_v1");
			for (const auto& Dependency : Dependencies)
			{
				UpdateCanonicalHash(Hash, static_cast<uint64>(Dependency.VirtualPath.size()));
				Hash.Update(Dependency.VirtualPath);
				UpdateCanonicalHash(Hash, Dependency.ContentHash);
			}
			OutFingerprint = {std::string(VirtualShaderPath), Hash.Finalize()};
			return {};
		}
		FShaderCompileOptions EffectiveOptions = Options;
		EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);
		EffectiveOptions.CompilerEnvironment = CompilerEnvironmentIdentity;
		std::vector<FShaderMacroDefinition> NormalizedMacros;
		if (auto Result = ShaderCompileUtilities::NormalizeMacros(EffectiveOptions, NormalizedMacros); !Result) return Result;
		FShaderDependencyKey DependencyKey;
		ShaderCompileUtilities::BuildDependencyKey(
			VirtualShaderPath, NormalizedMacros,
			EffectiveOptions.CompilerEnvironment, DependencyKey);
		const uint64 ReloadGeneration = GetShaderReloadGeneration();
		{
			std::lock_guard Lock(SourceTreeFingerprintCacheMutex);
			if (SourceTreeFingerprintCacheGeneration != ReloadGeneration)
			{
				SourceTreeFingerprintCache.clear();
				SourceTreeFingerprintCacheGeneration = ReloadGeneration;
			}
			if (const auto Found = SourceTreeFingerprintCache.find(
				DependencyKey.Hex);
				Found != SourceTreeFingerprintCache.end())
			{
				OutFingerprint = Found->second;
				SourceTreeFingerprintHits.fetch_add(
					1, std::memory_order_relaxed);
				return {};
			}
		}
		FShaderMetaData MetaData;
		if (auto Result = ResolveSourceMetaData(VirtualShaderPath, FShaderPaths::SourcePath(VirtualShaderPath), EffectiveOptions, NormalizedMacros, MetaData); !Result) return Result;
		OutFingerprint = {
			.VirtualPath = std::string(VirtualShaderPath),
			.ContentHash = MetaData.SourceTreeSignature};
		if (GetShaderReloadGeneration() == ReloadGeneration)
		{
			std::lock_guard Lock(SourceTreeFingerprintCacheMutex);
			if (SourceTreeFingerprintCacheGeneration == ReloadGeneration)
			{
				if (SourceTreeFingerprintCache.size()
					>= GMaximumSourceTreeFingerprintEntries)
					SourceTreeFingerprintCache.erase(
						SourceTreeFingerprintCache.begin());
				SourceTreeFingerprintCache.insert_or_assign(
					DependencyKey.Hex, OutFingerprint);
			}
		}
		return {};
	}

	auto FShaderBuilder::GetOrCompile(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput
	{
		FShaderCompilerOutput Output;
		if (VirtualShaderPath.empty())
		{
			Output.Error = {.Code = EShaderError::MissingVirtualPath};
			return Output;
		}
		if (!HasValidUniqueEntryPoints(Options))
		{
			Output.Error = {.Code = EShaderError::InvalidCompileRequest};
			return Output;
		}

		FShaderCompileOptions EffectiveOptions = Options;
		EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);
		EffectiveOptions.CompilerEnvironment = CompilerEnvironmentIdentity;

		std::vector<FShaderMacroDefinition> NormalizedMacros;
		if (auto Result = ShaderCompileUtilities::NormalizeMacros(EffectiveOptions, NormalizedMacros); !Result)
		{
			Output.Error = std::move(Result.error());
			return Output;
		}

		if (EffectiveOptions.SourceArtifacts)
		{
			std::string MountedIdentity;
			if (FShaderPaths::TryMakeVirtualSourcePath(VirtualShaderPath, MountedIdentity))
				EffectiveOptions.VirtualShaderPath = std::move(MountedIdentity);
			std::vector<std::string> Paths;
			if (auto Resolved = DependencyResolver.Resolve(EffectiveOptions.SourceArtifacts->GetFiles().contains(std::string(VirtualShaderPath))
				? std::string(VirtualShaderPath) : std::string(VirtualShaderPath) + ".slang", EffectiveOptions, Paths); !Resolved)
				return {.Error = std::move(Resolved.error())};
			auto MetaData = CapturedMetaData(std::move(Paths), *EffectiveOptions.SourceArtifacts);
			if (!MetaData) return {.Error = std::move(MetaData.error())};
			FShaderVariantKey Variant;
			ShaderCompileUtilities::BuildVariantKey(EffectiveOptions.VirtualShaderPath, *MetaData, NormalizedMacros, CompilerEnvironmentIdentity, Variant);
			return RunSingleFlight("Captured/" + BuildOutputKey(Variant, EffectiveOptions)
				+ (EffectiveOptions.bForceRecompile ? "/Forced" : "/Cached"), [&] {
				return ExecuteDerivedBuild(EffectiveOptions, Variant,
					[&]() -> FArtifactResult { return EffectiveOptions.SourceArtifacts; },
					[&](const FShaderCompileOptions& Prepared) { return Compiler.Compile(VirtualShaderPath, Prepared); });
			});
		}

		const std::string SourceFilePath = FShaderPaths::SourcePath(VirtualShaderPath);
		const std::string RequestKey = BuildRequestKey(VirtualShaderPath, EffectiveOptions, NormalizedMacros);
		return RunSingleFlight("Mounted/" + RequestKey, [&] {
			return GetOrCompileInternal(VirtualShaderPath, SourceFilePath,
				EffectiveOptions, NormalizedMacros);
		});
	}

	auto FShaderBuilder::GetStats() const -> FShaderBuildStats
	{
		std::lock_guard Lock(OutputCacheMutex);
		return FShaderBuildStats{
			.DependencyResolutions = DependencyResolutions.load(std::memory_order_relaxed),
			.ManifestHits = ManifestHits.load(std::memory_order_relaxed),
			.MemoryHits = MemoryHits.load(std::memory_order_relaxed),
			.DdcHits = DdcHits.load(std::memory_order_relaxed),
			.DdcCorruptMisses = DdcCorruptMisses.load(std::memory_order_relaxed),
			.DdcStoreFailures = DdcStoreFailures.load(std::memory_order_relaxed),
			.Compilations = Compilations.load(std::memory_order_relaxed),
			.ContentReads = FileFingerprintCache.GetContentReadCount(),
			.OutputEntries = OutputCache.size(),
			.SourceTreeFingerprintHits =
				SourceTreeFingerprintHits.load(std::memory_order_relaxed)
		};
	}

	auto FShaderBuilder::GetOrCompileGenerated(const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput
	{
		// Bound work before hashing/copying the generated root.
		if (Request.Source.size() > 1024 * 1024 || Request.EntryPoints.size() > 8)
			return {.Error = {.Code = EShaderError::InvalidGeneratedRequest}};
		if (!Request.SourceArtifacts) return GetOrCompileGeneratedInternal(Request);
		FShaderCompileOptions Options;
		Options.Frequencies = Request.Frequencies;
		Options.CompilerEnvironment = CompilerEnvironmentIdentity;
		Options.bForceRecompile = Request.bForceRecompile;
		Options.Macros = Request.Macros;
		for (const auto& Entry : Request.EntryPoints)
			Options.EntryPoints.push_back(Entry.c_str());
		if (!HasValidUniqueEntryPoints(Options))
			return {.Error = {.Code = EShaderError::InvalidCompileRequest}};
		std::vector<FShaderMacroDefinition> Macros;
		FShaderCompilerOutput Output;
		if (auto Result = ShaderCompileUtilities::NormalizeMacros(Options, Macros); !Result)
		{
			Output.Error = std::move(Result.error());
			return Output;
		}
		FXxHash128Builder Builder;
		UpdateCanonicalHashString(Builder, "GeneratedRequest_v1");
		UpdateCanonicalHashString(Builder, BuildRequestKey(Request.VirtualPath, Options, Macros));
		UpdateCanonicalHashString(Builder, Request.Source);
		auto Prefixes = Request.AllowedImportVirtualPrefixes;
		std::ranges::sort(Prefixes);
		Prefixes.erase(std::ranges::unique(Prefixes).begin(), Prefixes.end());
		UpdateCanonicalHash(Builder, static_cast<uint64>(Prefixes.size()));
		for (const auto& Prefix : Prefixes) UpdateCanonicalHashString(Builder, Prefix);
		// Content and ordered search roots, never the snapshot pointer.
		const auto& Files = Request.SourceArtifacts->GetFiles();
		UpdateCanonicalHash(Builder, static_cast<uint64>(Files.size()));
		for (const auto& [Path, Bytes] : Files)
		{
			UpdateCanonicalHashString(Builder, Path);
			UpdateCanonicalHash(Builder, FXxHash128::HashBuffer(FByteView(Bytes)));
		}
		const auto& Roots = Request.SourceArtifacts->GetSearchRoots();
		UpdateCanonicalHash(Builder, static_cast<uint64>(Roots.size()));
		for (const auto& Root : Roots) UpdateCanonicalHashString(Builder, Root);
		return RunSingleFlight("Generated/" + Builder.Finalize().ToString(), [&] {
			return GetOrCompileGeneratedInternal(Request);
		});
	}

	auto FShaderBuilder::GetOrCompileGeneratedInternal(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput
	{
		FShaderCompilerOutput Output;
		if (!Request.VirtualPath.starts_with("/Generated/Materials/")
			|| Request.Source.empty()
			|| Request.Source.size() > 1024 * 1024
			|| Request.EntryPoints.empty()
			|| Request.EntryPoints.size() != Request.Frequencies.size()
			|| Request.EntryPoints.size() > 8)
		{
			Output.Error = {.Code = EShaderError::InvalidGeneratedRequest};
			return Output;
		}
		FShaderCompileOptions Options;
		Options.Frequencies = Request.Frequencies;
		Options.SourceArtifacts = Request.SourceArtifacts;
		Options.Macros = Request.Macros;
		Options.bForceRecompile = Request.bForceRecompile;
		Options.VirtualShaderPath = Request.VirtualPath;
		Options.CompilerEnvironment = CompilerEnvironmentIdentity;
		Options.EntryPoints.reserve(Request.EntryPoints.size());
		for (const std::string& Entry : Request.EntryPoints)
			Options.EntryPoints.push_back(Entry.c_str());
		if (!HasValidUniqueEntryPoints(Options))
		{
			Output.Error = {.Code = EShaderError::InvalidCompileRequest};
			return Output;
		}

		std::vector<FShaderMacroDefinition> Macros;
		if (auto Result = ShaderCompileUtilities::NormalizeMacros(Options, Macros); !Result)
		{
			Output.Error = std::move(Result.error());
			return Output;
		}
		std::vector<std::string> AllowedImportVirtualPrefixes =
			Request.AllowedImportVirtualPrefixes;
		std::ranges::sort(AllowedImportVirtualPrefixes);
		AllowedImportVirtualPrefixes.erase(
			std::ranges::unique(AllowedImportVirtualPrefixes).begin(),
			AllowedImportVirtualPrefixes.end());
		if (Options.SourceArtifacts)
		{
			std::vector<std::string> Dependencies;
			if (auto Result = DependencyResolver.ResolveSource(Request.VirtualPath.substr(1), Request.VirtualPath, Request.Source, Options, Dependencies); !Result)
			{
				Output.Error = std::move(Result.error());
				return Output;
			}
			for (const auto& Path : Dependencies)
			{
				if (!Options.SourceArtifacts->GetFiles().contains(Path)
					|| !std::ranges::any_of(AllowedImportVirtualPrefixes,
						[&](const std::string& Prefix) { return Path.starts_with(Prefix); }))
				{
					Output.Error = {.Code = EShaderError::ImportNotDeclared, .ActualIdentity = Path};
					return Output;
				}
			}
			auto MetaData = CapturedMetaData(std::move(Dependencies), *Options.SourceArtifacts);
			if (!MetaData) return {.Error = std::move(MetaData.error())};
			FXxHash128Builder Tree;
			Tree.Update("DurinGeneratedShaderSourceTree_v1");
			UpdateCanonicalHash(Tree, FXxHash128::HashBuffer(Request.Source));
			UpdateCanonicalHash(Tree, MetaData->SourceTreeSignature);
			MetaData->SourceTreeSignature = Tree.Finalize();
			FShaderVariantKey Variant;
			ShaderCompileUtilities::BuildVariantKey(Request.VirtualPath, *MetaData, Macros, CompilerEnvironmentIdentity, Variant);
			return ExecuteDerivedBuild(Options, Variant,
				[&]() -> FArtifactResult { return Options.SourceArtifacts; },
				[&](const FShaderCompileOptions& Prepared) {
					return Compiler.CompileSource(Request.VirtualPath.substr(1), Request.VirtualPath,
						Request.Source, Prepared, BeforeGeneratedCompile);
				});
		}
		const FXxHash128 SourceHash = FXxHash128::HashBuffer(Request.Source);
		const auto& Mounts = FShaderPaths::GetRegisteredMountPoints();
		if (Mounts.empty())
		{
			Output.Error = {.Code = EShaderError::ShaderMountRequired};
			return Output;
		}
		const auto SourceMount = std::ranges::find_if(Mounts,
			[&](const FShaderPaths::FShaderMountPoint& Mount) {
				return std::ranges::any_of(
					AllowedImportVirtualPrefixes,
					[&](const std::string& Prefix) {
						return Prefix.starts_with(Mount.VirtualRoot);
					});
			});
		const std::string SourcePathHint =
			(std::filesystem::path(SourceMount != Mounts.end()
				? SourceMount->SourceDir : Mounts.front().SourceDir)
				/ "GeneratedMaterial.slang").generic_string();
		const std::string CachePath = std::format(
			"{}__GeneratedMaterials/{}", Mounts.front().VirtualRoot,
			SourceHash.ToString());
		FXxHash128Builder ImportContextBuilder;
		UpdateCanonicalHashString(
			ImportContextBuilder, "DurinGeneratedImportContext_v1");
		UpdateCanonicalHashString(ImportContextBuilder, SourcePathHint);
		UpdateCanonicalHash(ImportContextBuilder,
			static_cast<uint64>(AllowedImportVirtualPrefixes.size()));
		for (const std::string& Prefix : AllowedImportVirtualPrefixes)
			UpdateCanonicalHashString(ImportContextBuilder, Prefix);
		const std::string DependencyIdentity = std::format(
			"{}/Imports/{}", CachePath,
			ImportContextBuilder.Finalize().ToString());
		FShaderDependencyKey DependencyKey;
		ShaderCompileUtilities::BuildDependencyKey(
			DependencyIdentity, Macros, Options.CompilerEnvironment,
			DependencyKey);
		FShaderMetaData DependencyMetaData;
		bool bManifestCurrent = false;
		if (ManifestStore.Load(
			CachePath, DependencyKey, DependencyMetaData))
		{
			const ShaderCompileUtilities::FMetaDataReuseResult ReuseResult =
				ShaderCompileUtilities::TryReuseMetaData(
					DependencyMetaData, FileFingerprintCache);
			bManifestCurrent = ReuseResult.Status
				== ShaderCompileUtilities::EMetaDataReuseStatus::Current;
			if (ReuseResult.Status
				== ShaderCompileUtilities::EMetaDataReuseStatus::Failed)
			{
				DURIN_WARN(
					"Failed to validate generated shader dependency manifest for {}: {}",
					Request.VirtualPath, FormatShaderError(ReuseResult.Error));
			}
		}

		std::vector<std::string> DependencyPaths;
		if (bManifestCurrent)
		{
			DependencyPaths.reserve(DependencyMetaData.Dependencies.size());
			for (const FFileFingerprint& Dependency
				: DependencyMetaData.Dependencies)
				DependencyPaths.push_back(Dependency.NormalizedPath);
		}
		else
		{
			DependencyResolutions.fetch_add(1, std::memory_order_relaxed);
			if (auto Result = DependencyResolver.ResolveSource(Request.VirtualPath.substr(1), SourcePathHint, Request.Source, Options, DependencyPaths); !Result)
			{
				Output.Error = std::move(Result.error());
				return Output;
			}
			if (auto Result = ShaderCompileUtilities::BuildShaderMetaData(DependencyPaths, FileFingerprintCache, DependencyMetaData); !Result)
			{
				Output.Error = std::move(Result.error());
				return Output;
			}
		}
		if (auto Result = ValidateGeneratedImports(DependencyPaths, AllowedImportVirtualPrefixes); !Result)
		{
			Output.Error = std::move(Result.error());
			return Output;
		}
		if (!bManifestCurrent)
		{
			if (!ManifestStore.Save(
				CachePath, DependencyKey, DependencyMetaData))
				DURIN_WARN(
					"Generated shader dependency manifest write failed for {}",
					Request.VirtualPath);
		}
		else
		{
			ManifestHits.fetch_add(1, std::memory_order_relaxed);
		}

		FShaderMetaData MetaData = DependencyMetaData;
		FXxHash128Builder SourceTree;
		SourceTree.Update("DurinGeneratedShaderSourceTree_v1");
		UpdateCanonicalHash(SourceTree, SourceHash);
		UpdateCanonicalHash(SourceTree, MetaData.SourceTreeSignature);
		MetaData.SourceTreeSignature = SourceTree.Finalize();
		FShaderVariantKey VariantKey;
		ShaderCompileUtilities::BuildVariantKey(
			Request.VirtualPath, MetaData, Macros,
			Options.CompilerEnvironment, VariantKey);
		const std::string OutputKey = BuildOutputKey(VariantKey, Options);
		// Dependency content is now part of OutputKey. A source change during
		// another flight cannot join an obsolete compilation.
		return RunSingleFlight("GeneratedOutput/" + OutputKey
			+ (Options.bForceRecompile ? "/Forced" : "/Cached"), [&] {
			return ExecuteDerivedBuild(Options, VariantKey,
				[&] { return ShaderCompileUtilities::CaptureSourceArtifacts(DependencyMetaData); },
				[&](const FShaderCompileOptions& Prepared) {
					return Compiler.CompileSource(Request.VirtualPath.substr(1), Request.VirtualPath,
						Request.Source, Prepared, BeforeGeneratedCompile);
				});
		});
	}

	auto FShaderBuilder::ExecuteDerivedBuild(const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey,
		const std::function<FArtifactResult()>& Resolve,
		const std::function<FShaderCompilerOutput(const FShaderCompileOptions&)>& Compile) -> FShaderCompilerOutput
	{
		using namespace DerivedData;
		auto Definition = ShaderDerivedData::MakeBuildDefinition(VariantKey, Options);
		if (!Definition) return {.Error = std::move(Definition.error())};
		const auto OutputKey = Definition->GetKey().ToString();
		if (!Options.bForceRecompile)
		{
			std::lock_guard Lock(OutputCacheMutex);
			if (const auto Found = OutputCache.find(OutputKey); Found != OutputCache.end())
			{
				MemoryHits.fetch_add(1, std::memory_order_relaxed);
				OutputRecency.splice(OutputRecency.begin(), OutputRecency, Found->second.Recency);
				return Found->second.Output;
			}
		}
		FShaderBuildAdapter Adapter{.Options = Options, .Definition = *Definition,
			.Inputs = {FBuildInputReference{"Variant", VariantKey.Value, "ShaderVariant", 6, "Shader.SourceClosure", 1}},
			.ResolveArtifacts = Resolve, .Compile = [&](const FShaderCompileOptions& Prepared) {
				Compilations.fetch_add(1, std::memory_order_relaxed);
				return Compile(Prepared);
			}};
		TBuildObservations<FShaderError> Observations;
		auto Built = ExecuteBuild(*Definition, Adapter,
			{.bForceRebuild = Options.bForceRecompile, .MaximumValueBytes = ShaderDerivedData::MaximumValueBytes}, {}, Observations);
		if (Observations.DecodeError || (Observations.ReadError
			&& (Observations.ReadError->Code == ECacheError::ValueTooLarge || Observations.ReadError->Code == ECacheError::Corrupt)))
			DdcCorruptMisses.fetch_add(1, std::memory_order_relaxed);
		if (Observations.DecodeError) DURIN_WARN("Shader DDC value was rejected: {}", FormatShaderError(*Observations.DecodeError));
		if (Observations.EncodeError || Observations.WriteError)
		{
			DdcStoreFailures.fetch_add(1, std::memory_order_relaxed);
			DURIN_WARN("Shader DDC persistence failed: {}", Observations.EncodeError
				? FormatShaderError(*Observations.EncodeError) : Observations.WriteError->Diagnostic);
		}
		if (!Built) return {.Error = std::move(Built.error())};
		if (Observations.Origin == EBuildOrigin::CacheHit) DdcHits.fetch_add(1, std::memory_order_relaxed);
		AddOutput(OutputKey, *Built);
		return std::move(*Built);
	}

	auto FShaderBuilder::ValidateGeneratedImports(
		std::span<const std::string> DependencyPaths,
		std::span<const std::string> AllowedImportVirtualPrefixes) -> FShaderOperationResult
	{
		for (const std::string& PhysicalPath : DependencyPaths)
		{
			std::string VirtualPath;
			if (!FShaderPaths::TryMakeVirtualSourcePath(
				PhysicalPath, VirtualPath))
			{
				return std::unexpected(FShaderError{.Code = EShaderError::DependencyIdentityMissing, .ActualIdentity = PhysicalPath});
			}
			if (!std::ranges::any_of(
				AllowedImportVirtualPrefixes,
				[&](const std::string& Prefix) {
					return VirtualPath.starts_with(Prefix);
				}))
			{
				return std::unexpected(FShaderError{.Code = EShaderError::ImportNotAllowed, .ActualIdentity = VirtualPath});
			}
		}
		return {};
	}

	auto FShaderBuilder::AddOutput(std::string Key, const FShaderCompilerOutput& Output) -> void
	{
		std::lock_guard Lock(OutputCacheMutex);
		if (auto FoundIt = OutputCache.find(Key); FoundIt != OutputCache.end())
		{
			FoundIt->second.Output = Output;
			OutputRecency.splice(OutputRecency.begin(), OutputRecency, FoundIt->second.Recency);
			return;
		}
		OutputRecency.push_front(std::move(Key));
		OutputCache.emplace(OutputRecency.front(), FOutputCacheEntry{Output, OutputRecency.begin()});
		while (OutputCache.size() > GMaximumOutputEntries)
		{
			OutputCache.erase(OutputRecency.back());
			OutputRecency.pop_back();
		}
	}

	auto FShaderBuilder::ResolveSourceMetaData(
		std::string_view VirtualShaderPath,
		std::string_view SourceFilePath,
		const FShaderCompileOptions& EffectiveOptions,
		const std::vector<FShaderMacroDefinition>& NormalizedMacros,
		FShaderMetaData& OutMetaData) -> FShaderOperationResult
	{
		FShaderDependencyKey DependencyKey;
		ShaderCompileUtilities::BuildDependencyKey(
			VirtualShaderPath, NormalizedMacros,
			EffectiveOptions.CompilerEnvironment, DependencyKey);
		bool bManifestCurrent = false;
		if (ManifestStore.Load(
			VirtualShaderPath, DependencyKey, OutMetaData))
		{
			const ShaderCompileUtilities::FMetaDataReuseResult ReuseResult =
				ShaderCompileUtilities::TryReuseMetaData(
					OutMetaData, FileFingerprintCache);
			bManifestCurrent = ReuseResult.Status
				== ShaderCompileUtilities::EMetaDataReuseStatus::Current;
			if (ReuseResult.Status
				== ShaderCompileUtilities::EMetaDataReuseStatus::Failed)
			{
				DURIN_WARN(
					"Failed to validate shader dependency manifest for {}: {}",
					VirtualShaderPath, FormatShaderError(ReuseResult.Error));
			}
			if (bManifestCurrent)
				ManifestHits.fetch_add(1, std::memory_order_relaxed);
		}
		if (bManifestCurrent) return {};

		std::vector<std::string> DependencyPaths;
		DependencyResolutions.fetch_add(1, std::memory_order_relaxed);
		if (auto Result = DependencyResolver.Resolve(SourceFilePath, EffectiveOptions, DependencyPaths); !Result)
		{
			return Result;
		}
		if (auto Result = ShaderCompileUtilities::BuildShaderMetaData(DependencyPaths, FileFingerprintCache, OutMetaData); !Result)
			return Result;
		if (!ManifestStore.Save(
			VirtualShaderPath, DependencyKey, OutMetaData))
		{
			DURIN_WARN("Shader dependency manifest write failed for {}",
				VirtualShaderPath);
		}
		return {};
	}

	auto FShaderBuilder::GetOrCompileInternal(
		std::string_view VirtualShaderPath,
		std::string_view SourceFilePath,
		const FShaderCompileOptions& EffectiveOptions,
		const std::vector<FShaderMacroDefinition>& NormalizedMacros
	) -> FShaderCompilerOutput
	{
		FShaderCompilerOutput Output;
		FShaderMetaData CurrentMetaData;
		if (auto Result = ResolveSourceMetaData(VirtualShaderPath, SourceFilePath, EffectiveOptions, NormalizedMacros, CurrentMetaData); !Result)
		{
			Output.Error = std::move(Result.error());
			return Output;
		}

		FShaderVariantKey VariantKey;
		ShaderCompileUtilities::BuildVariantKey(VirtualShaderPath, CurrentMetaData, NormalizedMacros, EffectiveOptions.CompilerEnvironment, VariantKey);
		return ExecuteDerivedBuild(EffectiveOptions, VariantKey,
			[&] { return ShaderCompileUtilities::CaptureSourceArtifacts(CurrentMetaData); },
			[&](const FShaderCompileOptions& Prepared) { return Compiler.Compile(VirtualShaderPath, Prepared); });
	}
}
