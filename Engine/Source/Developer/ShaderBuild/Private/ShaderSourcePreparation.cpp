#include "ShaderBuilder.h"
#include "ShaderBuildVersion.h"
#include "Hash/CanonicalHash.h"

#include "ShaderCompileUtilities.h"
#include "Shader/ShaderCompiledOutput.h"
#include "ShaderDependencyManifestStore.h"
#include "SlangShaderDependencyResolver.h"

#include "Misc/FileFingerprintCache.h"
#include "Misc/FileHelper.h"
#include "ShaderBuild/ShaderPaths.h"

namespace Durin
{
	namespace
	{
		auto IsActiveCompilerTarget(const FShaderTargetIdentity& Target) -> bool
		{
			if (Target == VulkanShaderTarget) return true;
		#if defined(__APPLE__)
			return IsMetalShaderTarget(Target);
		#else
			return false;
		#endif
		}

		auto BuildRequestKey(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const std::vector<FShaderMacroDefinition>& Macros) -> std::string
		{
			FXxHash128Builder Builder;
			UpdateCanonicalHashString(Builder, "DurinShaderCompileRequest_v3");
			UpdateCanonicalHashString(Builder, VirtualShaderPath);
			UpdateCanonicalHashString(Builder, Options.CompilerEnvironment);
			UpdateCanonicalHash(Builder, ShaderBuildPrivate::BuildVersion(Options.CompilerEnvironment));
			UpdateCanonicalHash(Builder, Options.Target.Platform);
			UpdateCanonicalHash(Builder, Options.Target.Backend);
			UpdateCanonicalHash(Builder, Options.Target.IntermediateFormat);
			UpdateCanonicalHash(Builder, Options.Target.OutputFormat);
			UpdateCanonicalHash(Builder, Options.Target.SettingsDigest.HashLow);
			UpdateCanonicalHash(Builder, Options.Target.SettingsDigest.HashHigh);
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
			// In-process LRU/flight identity only. The session constructs its portable
			// action once on an LRU miss; force policy never changes this identity.
			FXxHash128Builder Hash;
			UpdateCanonicalHashString(Hash, "ShaderOutputMemory_v1");
			UpdateCanonicalHash(Hash, VariantKey.Value);
			UpdateCanonicalHash(Hash, uint64(Options.EntryPoints.size()));
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				UpdateCanonicalHashString(Hash, Options.EntryPoints[Index]);
				UpdateCanonicalHash(Hash, Options.Frequencies[Index]);
			}
			if (Options.SourceArtifacts)
			{
				const auto& Roots = Options.SourceArtifacts->GetSearchRoots();
				UpdateCanonicalHash(Hash, uint64(Roots.size()));
				for (const auto& Root : Roots) UpdateCanonicalHashString(Hash, Root);
			}
			else
			{
				const auto& Mounts = FShaderPaths::GetRegisteredMountPoints();
				UpdateCanonicalHash(Hash, uint64(Mounts.size()));
				for (const auto& Mount : Mounts) UpdateCanonicalHashString(Hash, Mount.VirtualRoot);
			}
			return Hash.Finalize().ToString();
		}

		auto HasValidUniqueEntryPoints(const FShaderCompileOptions& Options) -> bool
		{
			if (Options.EntryPoints.empty()
				|| Options.EntryPoints.size() != Options.Frequencies.size()
				|| Options.EntryPoints.size()
					> ShaderCompiledOutput::MaximumEntryPoints) return false;
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

		using FArtifactResult = std::expected<std::shared_ptr<const FShaderSourceArtifacts>, std::string>;

		auto CapturedMetaData(std::vector<std::string> Paths, const FShaderSourceArtifacts& Artifacts)
			-> std::expected<FShaderMetaData, std::string>
		{
			std::ranges::sort(Paths);
			Paths.erase(std::unique(Paths.begin(), Paths.end()), Paths.end());
			FShaderMetaData Result;
			for (const auto& Path : Paths)
			{
				const auto Found = Artifacts.GetFiles().find(Path);
				if (Found == Artifacts.GetFiles().end())
					return std::unexpected(std::format("Shader dependency '{}' was not captured.", (Path)));
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
					return std::unexpected(std::format("Shader virtual dependency '{}' resolves to conflicting content.", (Result.PortableDependencies[Index].VirtualPath)));
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


	}

	auto FShaderBuilder::BuildSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult
	{
		OutDependencies.clear();

		if (VirtualShaderPath.empty())
		{
			return std::unexpected(std::string("Shader type virtual shader path must not be empty"));
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
					return std::unexpected(std::format("Shader dependency '{}' was not captured.", (Path)));
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
				return std::unexpected(std::format("Shader dependency '{}' has no registered virtual identity.", (PhysicalPath)));
			}
			auto ContentHash = FFileHelper::HashFileXx128(PhysicalPath);
			if (!ContentHash)
			{
				OutDependencies.clear();
				return std::unexpected((std::move(ContentHash.error())).ToString());
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
			return std::unexpected(std::string("Shader type virtual shader path must not be empty"));
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
			EffectiveOptions.CompilerEnvironment, EffectiveOptions.Target, DependencyKey);
		const uint64 ReloadGeneration = GetShaderReloadGeneration();
		bool Fresh = false;
		{
			std::lock_guard Lock(SourceTreeFingerprintCacheMutex);
			if (SourceTreeFingerprintCacheGeneration != ReloadGeneration)
			{
				Fresh = true; FileFingerprintCache.Clear();
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
		if (auto Result = ResolveSourceMetaData(VirtualShaderPath, FShaderPaths::SourcePath(VirtualShaderPath), EffectiveOptions, NormalizedMacros, MetaData, Fresh); !Result) return Result;
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
		if (VirtualShaderPath.empty()) return FShaderCompilerOutput::Failure(std::string(std::string("Shader type virtual shader path must not be empty")));
		if (!IsActiveCompilerTarget(Options.Target) || !HasValidUniqueEntryPoints(Options)) return FShaderCompilerOutput::Failure(std::string(std::string("Shader entry-point request is invalid.")));
		FShaderCompilerOutput Output;
		auto Handle = PrepareMounted(VirtualShaderPath, Options, {.bInlineExecution = true}, [&](auto Value) { Output = std::move(Value); });
		Handle.Submit();
		require(Handle.Wait() == EShaderBuildWaitResult::Completed);
		return Output;
	}

	auto FShaderBuilder::PrepareMounted(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options,
		FShaderBuildRequestOptions RequestOptions, FShaderBuildCompletion Completion)
		-> FShaderBuildHandle
	{
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath = VirtualShaderPath; Request.Target = Options.Target; Request.Frequencies = Options.Frequencies;
		Request.Macros = Options.Macros; Request.SourceArtifacts = Options.SourceArtifacts; Request.bForceRecompile = Options.bForceRecompile;
		for (const auto* Entry : Options.EntryPoints) Request.EntryPoints.emplace_back(Entry ? Entry : "");
		return PrepareRequest(std::move(Request), std::move(RequestOptions), std::move(Completion), ESourceKind::Mounted);
	}

	auto FShaderBuilder::PrepareMounted(std::shared_ptr<const FGeneratedShaderCompileRequest> Request) -> std::expected<std::shared_ptr<FPreparedShaderBuild>, std::string>
	{
		if (Request->VirtualPath.empty() || !IsActiveCompilerTarget(Request->Target))
			return std::unexpected("Shader entry-point request is invalid.");
		auto Prepared = std::make_shared<FPreparedShaderBuild>(); Prepared->Request = std::move(Request);
		const auto& Input = *Prepared->Request; auto& Options = Prepared->Options;
		Options.Target = Input.Target; Options.VirtualShaderPath = Input.VirtualPath; Options.CompilerEnvironment = CompilerEnvironmentIdentity;
		Options.Frequencies = Input.Frequencies; Options.Macros = Input.Macros; Options.SourceArtifacts = Input.SourceArtifacts; Options.bForceRecompile = Input.bForceRecompile;
		for (const auto& Entry : Input.EntryPoints) Options.EntryPoints.push_back(Entry.c_str());
		if (!HasValidUniqueEntryPoints(Options)) return std::unexpected("Shader entry-point request is invalid.");
		std::vector<FShaderMacroDefinition> Macros;
		if (auto Result = ShaderCompileUtilities::NormalizeMacros(Options, Macros); !Result) return std::unexpected(Result.error());
		FShaderMetaData Meta; FShaderArtifactResolver Resolve;
		if (Options.SourceArtifacts)
		{
			std::string MountedIdentity;
			if (FShaderPaths::TryMakeVirtualSourcePath(Input.VirtualPath, MountedIdentity)) Options.VirtualShaderPath = std::move(MountedIdentity);
			std::vector<std::string> Paths;
			const auto Root = Options.SourceArtifacts->GetFiles().contains(Input.VirtualPath) ? Input.VirtualPath : Input.VirtualPath + ".slang";
			if (auto Result = DependencyResolver.Resolve(Root, Options, Paths); !Result) return std::unexpected(Result.error());
			auto Captured = CapturedMetaData(std::move(Paths), *Options.SourceArtifacts); if (!Captured) return std::unexpected(Captured.error());
			Meta = std::move(*Captured); Resolve = [Artifacts = Options.SourceArtifacts]() -> FArtifactResult { return Artifacts; };
		}
		else
		{
			// Only source discovery is serialized per request identity, never compilation or cache I/O.
			std::shared_ptr<std::mutex> Gate;
			{
				std::lock_guard Lock(SourcePreparationMutex);
				std::erase_if(SourcePreparationLocks, [](const auto& Item) { return Item.second.expired(); });
				auto& Weak = SourcePreparationLocks[BuildRequestKey(Input.VirtualPath, Options, Macros)]; Gate = Weak.lock();
				if (!Gate) { Gate = std::make_shared<std::mutex>(); Weak = Gate; }
			}
			std::lock_guard Lock(*Gate);
			if (auto Result = ResolveSourceMetaData(Input.VirtualPath, FShaderPaths::SourcePath(Input.VirtualPath), Options, Macros, Meta); !Result) return std::unexpected(Result.error());
			Resolve = [Meta] { return ShaderCompileUtilities::CaptureSourceArtifacts(Meta); };
		}
		FShaderVariantKey Variant;
		ShaderCompileUtilities::BuildVariantKey(Options.VirtualShaderPath, Meta, Macros, CompilerEnvironmentIdentity, Options.Target, Variant);
		Prepared->OutputKey = BuildOutputKey(Variant, Options);
		Options.Macros = std::move(Macros);
		auto Recipe = PrepareShaderSessionRecipe(Options, Variant, GetBuildVersion(), std::move(Meta.PortableDependencies), false, std::move(Resolve));
		if (!Recipe) return std::unexpected(Recipe.error()); Prepared->Recipe = std::move(*Recipe); return Prepared;
	}

	auto FShaderBuilder::PrepareGenerated(std::shared_ptr<const FGeneratedShaderCompileRequest> Owned)
		-> std::expected<std::shared_ptr<FPreparedShaderBuild>, std::string>
	{
		const auto& Request = *Owned;
		if (!IsActiveCompilerTarget(Request.Target)) return std::unexpected(std::string("Shader entry-point request is invalid."));
		if (!Request.VirtualPath.starts_with("/Generated/") || Request.Source.empty()
			|| Request.Source.size() > 1024 * 1024 || Request.EntryPoints.empty()
			|| Request.EntryPoints.size() != Request.Frequencies.size() || Request.EntryPoints.size() > 8)
			return std::unexpected(std::string("Generated shader request is invalid."));
		auto Prepared = std::make_shared<FPreparedShaderBuild>();
		Prepared->Request = std::move(Owned);
		auto& Options = Prepared->Options;
		Options.Target = Request.Target;
		Options.Frequencies = Request.Frequencies;
		Options.SourceArtifacts = Request.SourceArtifacts;
		Options.Macros = Request.Macros;
		Options.bForceRecompile = Request.bForceRecompile;
		Options.VirtualShaderPath = Request.VirtualPath;
		Options.CompilerEnvironment = CompilerEnvironmentIdentity;
		for (const auto& Entry : Request.EntryPoints) Options.EntryPoints.push_back(Entry.c_str());
		if (!HasValidUniqueEntryPoints(Options)) return std::unexpected(std::string("Shader entry-point request is invalid."));
		std::vector<FShaderMacroDefinition> Macros;
		if (auto Result = ShaderCompileUtilities::NormalizeMacros(Options, Macros); !Result) return std::unexpected(Result.error());
		auto Prefixes = Request.AllowedImportVirtualPrefixes;
		std::ranges::sort(Prefixes);
		Prefixes.erase(std::ranges::unique(Prefixes).begin(), Prefixes.end());
		// Material identity is bound to this exact captured environment. The dependency
		// resolver and compiler below read the same immutable bytes, including hits.
		if (Request.ExpectedSourceTree)
		{
			FShaderCompileOptions EnvironmentOptions;
			EnvironmentOptions.Target = Request.Target;
			EnvironmentOptions.VirtualShaderPath = Request.ExpectedSourceTree->VirtualPath;
			EnvironmentOptions.CompilerEnvironment = CompilerEnvironmentIdentity;
			for (const auto& Entry : Request.ExpectedSourceTree->EntryPoints) EnvironmentOptions.EntryPoints.push_back(Entry.c_str());
			EnvironmentOptions.Frequencies = Request.ExpectedSourceTree->Frequencies;
			EnvironmentOptions.Macros = Request.ExpectedSourceTree->Macros;
			if (EnvironmentOptions.EntryPoints.empty() || EnvironmentOptions.EntryPoints.size() > 8
				|| EnvironmentOptions.EntryPoints.size() != EnvironmentOptions.Frequencies.size() || !HasValidUniqueEntryPoints(EnvironmentOptions))
				return std::unexpected(std::string("Generated shader request is invalid."));
			FShaderMetaData EnvironmentMeta;
			if (Options.SourceArtifacts)
			{
				EnvironmentOptions.SourceArtifacts = Options.SourceArtifacts;
				std::vector<std::string> Paths;
				const auto Root = Options.SourceArtifacts->GetFiles().contains(EnvironmentOptions.VirtualShaderPath) ? EnvironmentOptions.VirtualShaderPath : EnvironmentOptions.VirtualShaderPath + ".slang";
				if (auto Result = DependencyResolver.Resolve(Root, EnvironmentOptions, Paths); !Result) return std::unexpected(Result.error());
				auto Captured = CapturedMetaData(std::move(Paths), *Options.SourceArtifacts);
				if (!Captured) return std::unexpected(Captured.error());
				EnvironmentMeta = std::move(*Captured);
			}
			else if (auto Result = ResolveSourceMetaData(EnvironmentOptions.VirtualShaderPath, FShaderPaths::SourcePath(EnvironmentOptions.VirtualShaderPath), EnvironmentOptions, EnvironmentOptions.Macros, EnvironmentMeta); !Result)
				return std::unexpected(Result.error());
			if (EnvironmentMeta.SourceTreeSignature != Request.ExpectedSourceTree->ContentHash)
				return std::unexpected(std::format("Shader virtual dependency '{}' resolves to conflicting content.", (std::string{})));
			if (!Options.SourceArtifacts)
			{
				auto Captured = ShaderCompileUtilities::CaptureSourceArtifacts(EnvironmentMeta);
				if (!Captured) return std::unexpected(Captured.error());
				Options.SourceArtifacts = std::move(*Captured);
			}
		}
		FShaderMetaData Meta;
		FShaderArtifactResolver Resolve;
		if (Options.SourceArtifacts)
		{
			std::vector<std::string> Paths;
			if (Request.ExpectedSourceTree && Request.ExpectedSourceTree->bCompleteClosure)
			{
				// The verified material environment is the complete declared closure.
				// The compiler filesystem has no fallback outside these immutable bytes.
				// Use the whole closure without re-running Slang dependency discovery.
				for (const auto& [Path, Data] : Options.SourceArtifacts->GetFiles())
					if (Path != Request.ExpectedSourceTree->VirtualPath && Path != Request.ExpectedSourceTree->VirtualPath + ".slang") Paths.push_back(Path);
			}
			else if (auto Result = DependencyResolver.ResolveSource(Request.VirtualPath.substr(1), Request.VirtualPath, Request.Source, Options, Paths); !Result)
				return std::unexpected(Result.error());
			for (const auto& Path : Paths)
				if (!Options.SourceArtifacts->GetFiles().contains(Path) || !std::ranges::any_of(Prefixes, [&](const auto& Prefix) { return Path.starts_with(Prefix); }))
					return std::unexpected(std::format("Generated shader import '{}' is not declared.", (Path)));
			auto Captured = CapturedMetaData(std::move(Paths), *Options.SourceArtifacts);
			if (!Captured) return std::unexpected(Captured.error());
			Meta = std::move(*Captured);
			Resolve = [Artifacts = Options.SourceArtifacts]() -> FArtifactResult { return Artifacts; };
		}
		else
		{
			const auto& Mounts = FShaderPaths::GetRegisteredMountPoints();
			if (Mounts.empty()) return std::unexpected(std::string("Generated shader compilation requires a registered shader mount"));
			const auto Mount = std::ranges::find_if(Mounts, [&](const auto& Item) {
				return std::ranges::any_of(Prefixes, [&](const auto& Prefix) { return Prefix.starts_with(Item.VirtualRoot); }); });
			const auto Hint = (std::filesystem::path(Mount != Mounts.end() ? Mount->SourceDir : Mounts.front().SourceDir) / "GeneratedMaterial.slang").generic_string();
			const auto CachePath = std::format("{}__GeneratedMaterials/{}", Mounts.front().VirtualRoot, FXxHash128::HashBuffer(Request.Source).ToString());
			FXxHash128Builder ImportHash;
			UpdateCanonicalHashString(ImportHash, "DurinGeneratedImportContext_v1"); UpdateCanonicalHashString(ImportHash, Hint);
			UpdateCanonicalHash(ImportHash, uint64(Prefixes.size())); for (const auto& Prefix : Prefixes) UpdateCanonicalHashString(ImportHash, Prefix);
			FShaderDependencyKey Key;
			ShaderCompileUtilities::BuildDependencyKey(std::format("{}/Imports/{}", CachePath, ImportHash.Finalize().ToString()), Macros, CompilerEnvironmentIdentity, Options.Target, Key);
			bool Current = ManifestStore.Load(CachePath, Key, Meta)
				&& ShaderCompileUtilities::TryReuseMetaData(Meta, FileFingerprintCache).Status == ShaderCompileUtilities::EMetaDataReuseStatus::Current;
			std::vector<std::string> Paths;
			if (Current)
			{
				ManifestHits.fetch_add(1);
				for (const auto& Dependency : Meta.Dependencies) Paths.push_back(Dependency.NormalizedPath);
			}
			else
			{
				DependencyResolutions.fetch_add(1);
				if (auto Result = DependencyResolver.ResolveSource(Request.VirtualPath.substr(1), Hint, Request.Source, Options, Paths); !Result) return std::unexpected(Result.error());
				if (auto Result = ShaderCompileUtilities::BuildShaderMetaData(Paths, FileFingerprintCache, Meta); !Result) return std::unexpected(Result.error());
			}
			if (auto Result = ValidateGeneratedImports(Paths, Prefixes); !Result) return std::unexpected(Result.error());
			if (!Current && !ManifestStore.Save(CachePath, Key, Meta)) DURIN_WARN("Generated shader manifest write failed for {}", Request.VirtualPath);
			Resolve = [CapturedMeta = Meta] { return ShaderCompileUtilities::CaptureSourceArtifacts(CapturedMeta); };
		}
		FXxHash128Builder Tree; Tree.Update("DurinGeneratedShaderSourceTree_v1");
		UpdateCanonicalHash(Tree, FXxHash128::HashBuffer(Request.Source)); UpdateCanonicalHash(Tree, Meta.SourceTreeSignature);
		Meta.SourceTreeSignature = Tree.Finalize();
		FShaderVariantKey Variant;
		ShaderCompileUtilities::BuildVariantKey(Request.VirtualPath, Meta, Macros, CompilerEnvironmentIdentity, Options.Target, Variant);
		Prepared->OutputKey = BuildOutputKey(Variant, Options);
		Options.Macros = std::move(Macros);
		auto Recipe = PrepareShaderSessionRecipe(Options, Variant, GetBuildVersion(), std::move(Meta.PortableDependencies), true, std::move(Resolve));
		if (!Recipe) return std::unexpected(Recipe.error());
		Prepared->Recipe = std::move(*Recipe);
		return Prepared;
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
				return std::unexpected(std::format("Shader dependency '{}' has no registered virtual identity.", (PhysicalPath)));
			}
			if (!std::ranges::any_of(
				AllowedImportVirtualPrefixes,
				[&](const std::string& Prefix) {
					return VirtualPath.starts_with(Prefix);
				}))
			{
				return std::unexpected(std::format("Generated shader import '{}' is not allowlisted.", (VirtualPath)));
			}
		}
		return {};
	}

	auto FShaderBuilder::ResolveSourceMetaData(
		std::string_view VirtualShaderPath,
		std::string_view SourceFilePath,
		const FShaderCompileOptions& EffectiveOptions,
		const std::vector<FShaderMacroDefinition>& NormalizedMacros,
		FShaderMetaData& OutMetaData, bool Fresh) -> FShaderOperationResult
	{
		FShaderDependencyKey DependencyKey;
		ShaderCompileUtilities::BuildDependencyKey(
			VirtualShaderPath, NormalizedMacros,
			EffectiveOptions.CompilerEnvironment, EffectiveOptions.Target, DependencyKey);
		bool bManifestCurrent = false;
		if (!Fresh && ManifestStore.Load(
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
					VirtualShaderPath, std::string(ReuseResult.Error));
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

}
