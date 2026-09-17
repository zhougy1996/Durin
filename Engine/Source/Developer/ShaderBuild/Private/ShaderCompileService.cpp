#include "ShaderCompileService.h"

#include "ShaderCompileUtilities.h"
#include "ShaderDerivedData.h"
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
		template <typename TBuilder>
		auto UpdateHashStringField(TBuilder& Builder, std::string_view Value) -> void
		{
			Builder.UpdateValue(static_cast<uint64>(Value.size()));
			Builder.Update(Value);
		}

		auto BuildRequestKey(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const std::vector<FShaderMacroDefinition>& Macros) -> std::string
		{
			FXxHash128Builder Builder;
			UpdateHashStringField(Builder, "DurinShaderCompileRequest_v1");
			UpdateHashStringField(Builder, VirtualShaderPath);
			UpdateHashStringField(Builder, Options.CompilerEnvironment);
			Builder.UpdateValue(Options.bForceRecompile);
			Builder.UpdateValue(static_cast<uint64>(Options.EntryPoints.size()));
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				UpdateHashStringField(Builder, Options.EntryPoints[Index] ? std::string_view(Options.EntryPoints[Index]) : std::string_view{});
				if (Index < Options.Frequencies.size()) Builder.UpdateValue(Options.Frequencies[Index]);
			}
			Builder.UpdateValue(static_cast<uint64>(Macros.size()));
			for (const FShaderMacroDefinition& Macro : Macros)
			{
				UpdateHashStringField(Builder, Macro.Name);
				Builder.UpdateValue(Macro.HasValue());
				if (Macro.Value) UpdateHashStringField(Builder, *Macro.Value);
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

		class FShaderCompileService
		{
		public:
			FShaderCompileService(std::function<void(std::string_view)> InBeforeGeneratedCompile)
				: CompilerEnvironmentIdentity(Compiler.GetEnvironmentIdentity())
				, BeforeGeneratedCompile(std::move(InBeforeGeneratedCompile))
			{
			}

			~FShaderCompileService()
			{
				FileFingerprintCache.Clear();
			}

			auto GetCompilerEnvironmentIdentity() const -> const std::string&
			{
				return CompilerEnvironmentIdentity;
			}

			auto BuildSourceDependencyManifest(
				std::string_view VirtualShaderPath,
				const FShaderCompileOptions& Options,
				std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult
			{
				OutDependencies.clear();

				if (VirtualShaderPath.empty())
				{
					return {.Error = {.Code = EShaderError::MissingVirtualPath}};
				}
				if (Options.SourceArtifacts)
				{
					std::vector<std::string> Paths;
					if (auto Result = DependencyResolver.Resolve(VirtualShaderPath, Options, Paths); !Result)
						return Result;
					for (const auto& Path : Paths)
					{
						const auto Found = Options.SourceArtifacts->GetFiles().find(Path);
						if (Found == Options.SourceArtifacts->GetFiles().end())
						{
							OutDependencies.clear();
							return {.Error = {.Code = EShaderError::DependencyNotCaptured, .ActualIdentity = Path}};
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
						return {.Error = {.Code = EShaderError::DependencyIdentityMissing, .ActualIdentity = PhysicalPath}};
					}
					FXxHash128 ContentHash;
					std::error_code ErrorCode;
					if (!FFileHelper::HashFileXx128(
						PhysicalPath, ContentHash, ErrorCode))
					{
						OutDependencies.clear();
						return FShaderOperationResult::FileSystemFailure(PhysicalPath, ErrorCode);
					}
					OutDependencies.push_back({
						.VirtualPath = std::move(VirtualPath),
						.ContentHash = ContentHash});
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

			auto BuildSourceTreeFingerprint(
				std::string_view VirtualShaderPath,
				const FShaderCompileOptions& Options,
				FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult
			{
				OutFingerprint = {};

				if (VirtualShaderPath.empty())
				{
					return {.Error = {.Code = EShaderError::MissingVirtualPath}};
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
						Hash.UpdateValue(static_cast<uint64>(Dependency.VirtualPath.size()));
						Hash.Update(Dependency.VirtualPath);
						Hash.UpdateValue(Dependency.ContentHash);
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

			auto GetOrCompile(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput
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

				if (Options.SourceArtifacts)
					return Compiler.Compile(VirtualShaderPath, Options);

				const std::string SourceFilePath = FShaderPaths::SourcePath(VirtualShaderPath);
				FShaderCompileOptions EffectiveOptions = Options;
				EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);
				EffectiveOptions.CompilerEnvironment = CompilerEnvironmentIdentity;

				std::vector<FShaderMacroDefinition> NormalizedMacros;
				if (auto Result = ShaderCompileUtilities::NormalizeMacros(EffectiveOptions, NormalizedMacros); !Result)
				{
					Output.Error = std::move(Result.Error);
					return Output;
				}

				const std::string RequestKey = BuildRequestKey(VirtualShaderPath, EffectiveOptions, NormalizedMacros);
				return RunSingleFlight("Mounted/" + RequestKey, [&] {
					return GetOrCompileInternal(VirtualShaderPath, SourceFilePath,
						EffectiveOptions, NormalizedMacros);
				});
			}

			auto GetStats() const -> FShaderCompileServiceStats
			{
				std::lock_guard Lock(OutputCacheMutex);
				return FShaderCompileServiceStats{
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

			auto GetOrCompileGenerated(const FGeneratedShaderCompileRequest& Request)
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
					Output.Error = std::move(Result.Error);
					return Output;
				}
				FXxHash128Builder Builder;
				UpdateHashStringField(Builder, "GeneratedRequest_v1");
				UpdateHashStringField(Builder, BuildRequestKey(Request.VirtualPath, Options, Macros));
				UpdateHashStringField(Builder, Request.Source);
				auto Prefixes = Request.AllowedImportVirtualPrefixes;
				std::ranges::sort(Prefixes);
				Prefixes.erase(std::ranges::unique(Prefixes).begin(), Prefixes.end());
				Builder.UpdateValue(static_cast<uint64>(Prefixes.size()));
				for (const auto& Prefix : Prefixes) UpdateHashStringField(Builder, Prefix);
				// Content and ordered search roots, never the snapshot pointer.
				const auto& Files = Request.SourceArtifacts->GetFiles();
				Builder.UpdateValue(static_cast<uint64>(Files.size()));
				for (const auto& [Path, Bytes] : Files)
				{
					UpdateHashStringField(Builder, Path);
					Builder.UpdateValue(FXxHash128::HashBuffer(FByteView(Bytes)));
				}
				const auto& Roots = Request.SourceArtifacts->GetSearchRoots();
				Builder.UpdateValue(static_cast<uint64>(Roots.size()));
				for (const auto& Root : Roots) UpdateHashStringField(Builder, Root);
				return RunSingleFlight("Generated/" + Builder.Finalize().ToString(), [&] {
					return GetOrCompileGeneratedInternal(Request);
				});
			}

			auto GetOrCompileGeneratedInternal(
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
					Output.Error = std::move(Result.Error);
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
						Output.Error = std::move(Result.Error);
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
					return Compiler.CompileSource(Request.VirtualPath.substr(1),
						Request.VirtualPath, Request.Source, Options, BeforeGeneratedCompile);
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
				UpdateHashStringField(
					ImportContextBuilder, "DurinGeneratedImportContext_v1");
				UpdateHashStringField(ImportContextBuilder, SourcePathHint);
				ImportContextBuilder.UpdateValue(
					static_cast<uint64>(AllowedImportVirtualPrefixes.size()));
				for (const std::string& Prefix : AllowedImportVirtualPrefixes)
					UpdateHashStringField(ImportContextBuilder, Prefix);
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
						Output.Error = std::move(Result.Error);
						return Output;
					}
					if (auto Result = ShaderCompileUtilities::BuildShaderMetaData(DependencyPaths, FileFingerprintCache, DependencyMetaData); !Result)
					{
						Output.Error = std::move(Result.Error);
						return Output;
					}
				}
				if (auto Result = ValidateGeneratedImports(DependencyPaths, AllowedImportVirtualPrefixes); !Result)
				{
					Output.Error = std::move(Result.Error);
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
				SourceTree.UpdateValue(SourceHash);
				SourceTree.UpdateValue(MetaData.SourceTreeSignature);
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
					if (!Options.bForceRecompile)
					{
						std::lock_guard Lock(OutputCacheMutex);
						if (const auto Found = OutputCache.find(OutputKey);
							Found != OutputCache.end())
						{
							MemoryHits.fetch_add(1, std::memory_order_relaxed);
							OutputRecency.splice(OutputRecency.begin(),
								OutputRecency, Found->second.Recency);
							return Found->second.Output;
						}
					}
					if (!Options.bForceRecompile && TryLoadDerivedData(
						Options, VariantKey, Output))
					{
						DdcHits.fetch_add(1, std::memory_order_relaxed);
						AddOutput(OutputKey, Output);
						return Output;
					}
					Compilations.fetch_add(1, std::memory_order_relaxed);
					Output = Compiler.CompileSource(Request.VirtualPath.substr(1),
						SourcePathHint, Request.Source, Options, BeforeGeneratedCompile);
					if (!Output) return Output;
					StoreDerivedData(Options, VariantKey, Output);
					AddOutput(OutputKey, Output);
					return Output;
				});
			}

		private:
			static constexpr size_t GMaximumOutputEntries = 128;
			static constexpr size_t GMaximumSourceTreeFingerprintEntries = 128;

			auto TryLoadDerivedData(const FShaderCompileOptions& Options,
				const FShaderVariantKey& VariantKey,
				FShaderCompilerOutput& OutOutput) -> bool
			{
				using namespace DerivedData;
				const FCacheKey Key = ShaderDerivedData::BuildKey(
					VariantKey, Options);
				if (!Key.IsValid()) return false;
				const FCacheGetResult Result = DerivedData::GetCache().Get({
					Key,
					ShaderDerivedData::MaximumValueBytes});
				if (Result.Status != ECacheGetStatus::Hit)
				{
					if (Result.Status == ECacheGetStatus::ValueTooLarge
						|| Result.Status == ECacheGetStatus::Corrupt)
						DdcCorruptMisses.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
				if (const auto DecodeResult = ShaderDerivedData::Decode(Result.Value.GetBytes(), Options, OutOutput); !DecodeResult)
				{
					DdcCorruptMisses.fetch_add(1, std::memory_order_relaxed);
					DURIN_WARN("Shader DDC value was rejected: {}", FormatShaderError(DecodeResult.Error));
					return false;
				}
				return true;
			}

			auto StoreDerivedData(const FShaderCompileOptions& Options,
				const FShaderVariantKey& VariantKey,
				const FShaderCompilerOutput& Output) -> void
			{
				using namespace DerivedData;
				FByteBuffer Bytes;
				const FCacheKey Key = ShaderDerivedData::BuildKey(
					VariantKey, Options);
				if (!Key.IsValid())
				{
					DdcStoreFailures.fetch_add(1, std::memory_order_relaxed);
					DURIN_WARN("Shader DDC encoding skipped: invalid cache key.");
					return;
				}
				if (const auto EncodeResult = ShaderDerivedData::Encode(Options, Output, Bytes); !EncodeResult)
				{
					DdcStoreFailures.fetch_add(1, std::memory_order_relaxed);
					DURIN_WARN("Shader DDC encoding failed: {}", FormatShaderError(EncodeResult.Error));
					return;
				}
				const FCachePutResult Put = DerivedData::GetCache().Put({
					Key, Bytes, ShaderDerivedData::MaximumValueBytes});
				if (!Put)
				{
					DdcStoreFailures.fetch_add(1, std::memory_order_relaxed);
					DURIN_WARN("Shader DDC store failed: {}", Put.Diagnostic);
				}
			}

			static auto ValidateGeneratedImports(
				std::span<const std::string> DependencyPaths,
				std::span<const std::string> AllowedImportVirtualPrefixes) -> FShaderOperationResult
			{
				for (const std::string& PhysicalPath : DependencyPaths)
				{
					std::string VirtualPath;
					if (!FShaderPaths::TryMakeVirtualSourcePath(
						PhysicalPath, VirtualPath))
					{
						return {.Error = {.Code = EShaderError::DependencyIdentityMissing, .ActualIdentity = PhysicalPath}};
					}
					if (!std::ranges::any_of(
						AllowedImportVirtualPrefixes,
						[&](const std::string& Prefix) {
							return VirtualPath.starts_with(Prefix);
						}))
					{
						return {.Error = {.Code = EShaderError::ImportNotAllowed, .ActualIdentity = VirtualPath}};
					}
				}
				return {};
			}

			struct FOutputCacheEntry
			{
				FShaderCompilerOutput Output;
				std::list<std::string>::iterator Recency;
			};

			auto AddOutput(std::string Key, const FShaderCompilerOutput& Output) -> void
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

			struct FInFlightRequest
			{
				std::condition_variable Condition;
				bool bCompleted = false;
				FShaderCompilerOutput Output;
				std::exception_ptr Exception;
			};

			template <typename F>
			auto RunSingleFlight(const std::string& Key, F&& Compile) -> FShaderCompilerOutput
			{
				std::shared_ptr<FInFlightRequest> Flight;
				{
					std::unique_lock Lock(InFlightMutex);
					if (const auto Found = InFlightRequests.find(Key); Found != InFlightRequests.end())
					{
						Flight = Found->second;
						Flight->Condition.wait(Lock, [&] { return Flight->bCompleted; });
						Lock.unlock();
						if (Flight->Exception) std::rethrow_exception(Flight->Exception);
						return Flight->Output;
					}
					Flight = std::make_shared<FInFlightRequest>();
					InFlightRequests.emplace(Key, Flight);
				}
				try { Flight->Output = Compile(); }
				catch (...) { Flight->Exception = std::current_exception(); }
				{
					std::lock_guard Lock(InFlightMutex);
					Flight->bCompleted = true;
					InFlightRequests.erase(Key);
				}
				Flight->Condition.notify_all();
				if (Flight->Exception) std::rethrow_exception(Flight->Exception);
				return Flight->Output;
			}

			auto ResolveSourceMetaData(
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

			auto GetOrCompileInternal(
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
					Output.Error = std::move(Result.Error);
					return Output;
				}

				FShaderVariantKey VariantKey;
				ShaderCompileUtilities::BuildVariantKey(VirtualShaderPath, CurrentMetaData, NormalizedMacros, EffectiveOptions.CompilerEnvironment, VariantKey);
				const std::string OutputKey = BuildOutputKey(VariantKey, EffectiveOptions);

				if (!EffectiveOptions.bForceRecompile)
				{
					std::lock_guard Lock(OutputCacheMutex);
					if (const auto FoundIt = OutputCache.find(OutputKey); FoundIt != OutputCache.end())
					{
						MemoryHits.fetch_add(1, std::memory_order_relaxed);
						OutputRecency.splice(OutputRecency.begin(), OutputRecency, FoundIt->second.Recency);
						return FoundIt->second.Output;
					}
				}

				if (!EffectiveOptions.bForceRecompile
					&& TryLoadDerivedData(EffectiveOptions, VariantKey, Output))
				{
					DdcHits.fetch_add(1, std::memory_order_relaxed);
					AddOutput(OutputKey, Output);
					return Output;
				}

				Compilations.fetch_add(1, std::memory_order_relaxed);
				Output = Compiler.Compile(SourceFilePath, EffectiveOptions);
				if (!Output)
				{
					return Output;
				}

				DURIN_DEBUG("Shader compiled (Virtual: {}, Hash: {})", VirtualShaderPath, VariantKey.Hex);

				StoreDerivedData(EffectiveOptions, VariantKey, Output);
				AddOutput(OutputKey, Output);

				return Output;
			}

			FSlangShaderCompiler Compiler;
			FSlangShaderDependencyResolver DependencyResolver;
			const std::string CompilerEnvironmentIdentity;
			FShaderDependencyManifestStore ManifestStore;
			FFileFingerprintCache FileFingerprintCache;
			std::mutex InFlightMutex;
			std::unordered_map<std::string, std::shared_ptr<FInFlightRequest>> InFlightRequests;
			const std::function<void(std::string_view)> BeforeGeneratedCompile;
			mutable std::mutex OutputCacheMutex;
			std::list<std::string> OutputRecency;
			std::unordered_map<std::string, FOutputCacheEntry> OutputCache;
			std::mutex SourceTreeFingerprintCacheMutex;
			uint64 SourceTreeFingerprintCacheGeneration = 0;
			std::unordered_map<std::string,
				FShaderSourceDependencyFingerprint> SourceTreeFingerprintCache;
			std::atomic_uint64_t DependencyResolutions = 0;
			std::atomic_uint64_t ManifestHits = 0;
			std::atomic_uint64_t MemoryHits = 0;
			std::atomic_uint64_t DdcHits = 0;
			std::atomic_uint64_t DdcCorruptMisses = 0;
			std::atomic_uint64_t DdcStoreFailures = 0;
			std::atomic_uint64_t Compilations = 0;
			std::atomic_uint64_t SourceTreeFingerprintHits = 0;
		};

		std::unique_ptr<FShaderCompileService> GShaderCompileService;
	}

	auto InitShaderCompileService(std::function<void(std::string_view)> BeforeGeneratedCompile) -> void
	{
		GShaderCompileService = std::make_unique<FShaderCompileService>(std::move(BeforeGeneratedCompile));
	}

	auto ShutdownShaderCompileService() -> void
	{
		GShaderCompileService.reset();
	}

	auto GetOrCompileShader(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput
	{
		if (!GShaderCompileService)
		{
			FShaderCompilerOutput Output;
			Output.Error = {.Code = EShaderError::CompileServiceUnavailable};
			return Output;
		}

		return GShaderCompileService->GetOrCompile(VirtualShaderPath, Options);
	}

	auto GetShaderCompileServiceStats() -> FShaderCompileServiceStats
	{
		return GShaderCompileService ? GShaderCompileService->GetStats() : FShaderCompileServiceStats{};
	}

	auto GetShaderCompilerEnvironmentIdentityFromService() -> std::string
	{
		return GShaderCompileService
			? GShaderCompileService->GetCompilerEnvironmentIdentity()
			: std::string{};
	}

	auto BuildShaderSourceDependencyManifestFromService(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult
	{
		if (!GShaderCompileService)
		{
			OutDependencies.clear();
			return {.Error = {.Code = EShaderError::CompileServiceUnavailable}};
		}
		return GShaderCompileService->BuildSourceDependencyManifest(VirtualShaderPath, Options, OutDependencies);
	}

	auto BuildShaderSourceTreeFingerprintFromService(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult
	{
		if (!GShaderCompileService)
		{
			OutFingerprint = {};
			return {.Error = {.Code = EShaderError::CompileServiceUnavailable}};
		}
		return GShaderCompileService->BuildSourceTreeFingerprint(VirtualShaderPath, Options, OutFingerprint);
	}

	auto GetOrCompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput
	{
		if (!GShaderCompileService)
		{
			FShaderCompilerOutput Output;
			Output.Error = {.Code = EShaderError::CompileServiceUnavailable};
			return Output;
		}
		return GShaderCompileService->GetOrCompileGenerated(Request);
	}

}
