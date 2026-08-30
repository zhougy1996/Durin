#include "ShaderCompileService.h"

#include "ShaderCompileUtilities.h"
#include "ShaderDerivedData.h"
#include "ShaderDependencyManifestStore.h"
#include "SlangShaderCompiler.h"
#include "SlangShaderDependencyResolver.h"

#include "Misc/FileFingerprintCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Shader/ShaderPaths.h"

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
			FShaderCompileService()
				: CompilerEnvironmentIdentity(Compiler.GetEnvironmentIdentity())
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
				std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
				std::string& OutError) -> bool
			{
				OutDependencies.clear();
				OutError.clear();
				if (VirtualShaderPath.empty())
				{
					OutError = "Shader dependency manifest requires a virtual root path.";
					return false;
				}
				FShaderCompileOptions EffectiveOptions = Options;
				EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);
				EffectiveOptions.CompilerEnvironment = CompilerEnvironmentIdentity;
				std::vector<std::string> PhysicalDependencies;
				if (!DependencyResolver.Resolve(
					FShaderPaths::SourcePath(VirtualShaderPath), EffectiveOptions,
					PhysicalDependencies, OutError)) return false;

				OutDependencies.reserve(PhysicalDependencies.size());
				for (const std::string& PhysicalPath : PhysicalDependencies)
				{
					std::string VirtualPath;
					if (!FShaderPaths::TryMakeVirtualSourcePath(
						PhysicalPath, VirtualPath))
					{
						OutError = "Shader dependency cannot be represented by a registered virtual path.";
						OutDependencies.clear();
						return false;
					}
					FXxHash128 ContentHash;
					std::error_code ErrorCode;
					if (!FFileHelper::HashFileXx128(
						PhysicalPath, ContentHash, ErrorCode))
					{
						OutError = std::format(
							"Failed to fingerprint shader dependency {}: {}",
							VirtualPath, ErrorCode.message());
						OutDependencies.clear();
						return false;
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
				return true;
			}

			auto BuildSourceTreeFingerprint(
				std::string_view VirtualShaderPath,
				const FShaderCompileOptions& Options,
				FShaderSourceDependencyFingerprint& OutFingerprint,
				std::string& OutError) -> bool
			{
				OutFingerprint = {};
				OutError.clear();
				if (VirtualShaderPath.empty())
				{
					OutError = "Shader source tree fingerprint requires a virtual root path.";
					return false;
				}
				FShaderCompileOptions EffectiveOptions = Options;
				EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);
				EffectiveOptions.CompilerEnvironment = CompilerEnvironmentIdentity;
				std::vector<FShaderMacroDefinition> NormalizedMacros;
				if (!ShaderCompileUtilities::NormalizeMacros(
					EffectiveOptions, NormalizedMacros, OutError)) return false;
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
						return true;
					}
				}
				FShaderMetaData MetaData;
				if (!ResolveSourceMetaData(
					VirtualShaderPath, FShaderPaths::SourcePath(VirtualShaderPath),
					EffectiveOptions, NormalizedMacros, MetaData, OutError)) return false;
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
				return true;
			}

			auto GetOrCompile(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput
			{
				FShaderCompilerOutput Output;
				if (VirtualShaderPath.empty())
				{
					Output.ErrorMessage = "Virtual shader path is required for shader compile service";
					return Output;
				}
				if (!HasValidUniqueEntryPoints(Options))
				{
					Output.ErrorMessage = "Shader compile request entry points and frequencies must be valid, unique, bounded, and have matching counts";
					return Output;
				}

				const std::string SourceFilePath = FShaderPaths::SourcePath(VirtualShaderPath);
				FShaderCompileOptions EffectiveOptions = Options;
				EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);
				EffectiveOptions.CompilerEnvironment = CompilerEnvironmentIdentity;

				std::vector<FShaderMacroDefinition> NormalizedMacros;
				if (!ShaderCompileUtilities::NormalizeMacros(EffectiveOptions, NormalizedMacros, Output.ErrorMessage))
				{
					return Output;
				}

				const std::string RequestKey = BuildRequestKey(VirtualShaderPath, EffectiveOptions, NormalizedMacros);
				std::shared_ptr<FInFlightRequest> InFlight;
				bool bOwner = false;
				{
					std::unique_lock Lock(InFlightMutex);
					if (const auto FoundIt = InFlightRequests.find(RequestKey); FoundIt != InFlightRequests.end())
					{
						InFlight = FoundIt->second;
						InFlight->Condition.wait(Lock, [&InFlight] { return InFlight->bCompleted; });
						return InFlight->Output;
					}
					InFlight = std::make_shared<FInFlightRequest>();
					InFlightRequests.emplace(RequestKey, InFlight);
					bOwner = true;
				}

				check(bOwner);
				Output = GetOrCompileInternal(VirtualShaderPath, SourceFilePath, EffectiveOptions, NormalizedMacros);
				{
					std::lock_guard Lock(InFlightMutex);
					InFlight->Output = Output;
					InFlight->bCompleted = true;
					InFlightRequests.erase(RequestKey);
				}
				InFlight->Condition.notify_all();
				return Output;
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
					.DdcMaintenanceFailures = DdcMaintenanceFailures.load(std::memory_order_relaxed),
					.Compilations = Compilations.load(std::memory_order_relaxed),
					.ContentReads = FileFingerprintCache.GetContentReadCount(),
					.OutputEntries = OutputCache.size(),
					.SourceTreeFingerprintHits =
						SourceTreeFingerprintHits.load(std::memory_order_relaxed)
				};
			}

			auto GetOrCompileGenerated(
				const FGeneratedShaderCompileRequest& Request)
				-> FShaderCompilerOutput
			{
				std::lock_guard CompileLock(GeneratedCompileMutex);
				FShaderCompilerOutput Output;
				if (!Request.VirtualPath.starts_with("/Generated/Materials/")
					|| Request.Source.empty()
					|| Request.Source.size() > 1024 * 1024
					|| Request.EntryPoints.empty()
					|| Request.EntryPoints.size() != Request.Frequencies.size()
					|| Request.EntryPoints.size() > 8)
				{
					Output.ErrorMessage = "Invalid generated shader compile request";
					return Output;
				}
				FShaderCompileOptions Options;
				Options.Frequencies = Request.Frequencies;
				Options.Macros = Request.Macros;
				Options.bForceRecompile = Request.bForceRecompile;
				Options.VirtualShaderPath = Request.VirtualPath;
				Options.CompilerEnvironment = CompilerEnvironmentIdentity;
				Options.EntryPoints.reserve(Request.EntryPoints.size());
				for (const std::string& Entry : Request.EntryPoints)
					Options.EntryPoints.push_back(Entry.c_str());
				if (!HasValidUniqueEntryPoints(Options))
				{
					Output.ErrorMessage = "Invalid generated shader entry-point request";
					return Output;
				}

				std::vector<FShaderMacroDefinition> Macros;
				if (!ShaderCompileUtilities::NormalizeMacros(
					Options, Macros, Output.ErrorMessage)) return Output;
				std::vector<std::string> AllowedImportVirtualPrefixes =
					Request.AllowedImportVirtualPrefixes;
				std::ranges::sort(AllowedImportVirtualPrefixes);
				AllowedImportVirtualPrefixes.erase(
					std::ranges::unique(AllowedImportVirtualPrefixes).begin(),
					AllowedImportVirtualPrefixes.end());
				const FXxHash128 SourceHash = FXxHash128::HashBuffer(Request.Source);
				const auto& Mounts = FShaderPaths::GetRegisteredMountPoints();
				if (Mounts.empty())
				{
					Output.ErrorMessage = "Generated shader compilation requires a registered shader mount";
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
					std::string ManifestError;
					if (!ShaderCompileUtilities::TryReuseMetaData(
						DependencyMetaData, FileFingerprintCache,
						bManifestCurrent, ManifestError))
					{
						DURIN_WARN(
							"Failed to validate generated shader dependency manifest for {}: {}",
							Request.VirtualPath, ManifestError);
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
					if (!DependencyResolver.ResolveSource(
						Request.VirtualPath.substr(1), SourcePathHint,
						Request.Source, Options, DependencyPaths,
						Output.ErrorMessage)) return Output;
					if (!ShaderCompileUtilities::BuildShaderMetaData(
						DependencyPaths, FileFingerprintCache,
						DependencyMetaData, Output.ErrorMessage)) return Output;
				}
				if (!ValidateGeneratedImports(
					DependencyPaths, AllowedImportVirtualPrefixes,
					Output.ErrorMessage)) return Output;
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
					SourcePathHint, Request.Source, Options);
				if (!Output) return Output;
				StoreDerivedData(Options, VariantKey, Output);
				AddOutput(OutputKey, Output);
				return Output;
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
				const FCacheGetResult Result = GetDerivedDataCache().Get({
					ShaderDerivedData::GetBucket(), Key,
					ShaderDerivedData::MaximumValueBytes});
				if (Result.Status != ECacheGetStatus::Hit)
				{
					if (Result.Status == ECacheGetStatus::ValueTooLarge)
						DdcCorruptMisses.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
				std::string Error;
				if (!ShaderDerivedData::Decode(
					Result.Value.GetBytes(), Options, OutOutput, Error))
				{
					DdcCorruptMisses.fetch_add(1, std::memory_order_relaxed);
					DURIN_WARN("Shader DDC value was rejected: {}", Error);
					return false;
				}
				return true;
			}

			auto StoreDerivedData(const FShaderCompileOptions& Options,
				const FShaderVariantKey& VariantKey,
				const FShaderCompilerOutput& Output) -> void
			{
				using namespace DerivedData;
				std::vector<std::byte> Bytes;
				std::string Error;
				const FCacheKey Key = ShaderDerivedData::BuildKey(
					VariantKey, Options);
				if (!Key.IsValid() || !ShaderDerivedData::Encode(
					Options, Output, Bytes, Error))
				{
					DdcStoreFailures.fetch_add(1, std::memory_order_relaxed);
					DURIN_WARN("Shader DDC encoding failed: {}", Error);
					return;
				}
				const FCacheBucket Bucket = ShaderDerivedData::GetBucket();
				const FCachePutResult Put = GetDerivedDataCache().Put({
					Bucket, Key, Bytes, ShaderDerivedData::MaximumValueBytes});
				if (!Put)
				{
					DdcStoreFailures.fetch_add(1, std::memory_order_relaxed);
					DURIN_WARN("Shader DDC store failed: {}", Put.Diagnostic);
				}
				const FCacheTrimResult Trim = GetDerivedDataCache().Trim({
					Bucket, ShaderDerivedData::BucketBudgetBytes,
					ShaderDerivedData::CleanupDeleteLimit});
				if (Trim.Status != ECacheTrimStatus::Complete)
				{
					DdcMaintenanceFailures.fetch_add(1, std::memory_order_relaxed);
					DURIN_WARN("Shader DDC maintenance did not complete: {}",
						Trim.Diagnostic);
				}
			}

			static auto ValidateGeneratedImports(
				std::span<const std::string> DependencyPaths,
				std::span<const std::string> AllowedImportVirtualPrefixes,
				std::string& OutErrorMessage) -> bool
			{
				for (const std::string& PhysicalPath : DependencyPaths)
				{
					std::string VirtualPath;
					if (!FShaderPaths::TryMakeVirtualSourcePath(
						PhysicalPath, VirtualPath))
					{
						OutErrorMessage =
							"Generated shader import has no virtual identity";
						return false;
					}
					if (!std::ranges::any_of(
						AllowedImportVirtualPrefixes,
						[&](const std::string& Prefix) {
							return VirtualPath.starts_with(Prefix);
						}))
					{
						OutErrorMessage = std::format(
							"Generated shader import is not allowlisted: {}",
							VirtualPath);
						return false;
					}
				}
				return true;
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
			};

			auto ResolveSourceMetaData(
				std::string_view VirtualShaderPath,
				std::string_view SourceFilePath,
				const FShaderCompileOptions& EffectiveOptions,
				const std::vector<FShaderMacroDefinition>& NormalizedMacros,
				FShaderMetaData& OutMetaData,
				std::string& OutError) -> bool
			{
				FShaderDependencyKey DependencyKey;
				ShaderCompileUtilities::BuildDependencyKey(
					VirtualShaderPath, NormalizedMacros,
					EffectiveOptions.CompilerEnvironment, DependencyKey);
				bool bManifestCurrent = false;
				if (ManifestStore.Load(
					VirtualShaderPath, DependencyKey, OutMetaData))
				{
					std::string ManifestError;
					if (!ShaderCompileUtilities::TryReuseMetaData(
						OutMetaData, FileFingerprintCache,
						bManifestCurrent, ManifestError))
					{
						DURIN_WARN(
							"Failed to validate shader dependency manifest for {}: {}",
							VirtualShaderPath, ManifestError);
					}
					if (bManifestCurrent)
						ManifestHits.fetch_add(1, std::memory_order_relaxed);
				}
				if (bManifestCurrent) return true;

				std::vector<std::string> DependencyPaths;
				DependencyResolutions.fetch_add(1, std::memory_order_relaxed);
				if (!DependencyResolver.Resolve(
					SourceFilePath, EffectiveOptions, DependencyPaths, OutError))
				{
					if (OutError.empty()) OutError = "Failed to parse shader dependency graph";
					return false;
				}
				if (!ShaderCompileUtilities::BuildShaderMetaData(
					DependencyPaths, FileFingerprintCache, OutMetaData, OutError))
					return false;
				if (!ManifestStore.Save(
					VirtualShaderPath, DependencyKey, OutMetaData))
				{
					DURIN_WARN("Shader dependency manifest write failed for {}",
						VirtualShaderPath);
				}
				return true;
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
				if (!ResolveSourceMetaData(VirtualShaderPath, SourceFilePath,
					EffectiveOptions, NormalizedMacros, CurrentMetaData,
					Output.ErrorMessage)) return Output;

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
			std::mutex GeneratedCompileMutex;
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
			std::atomic_uint64_t DdcMaintenanceFailures = 0;
			std::atomic_uint64_t Compilations = 0;
			std::atomic_uint64_t SourceTreeFingerprintHits = 0;
		};

		std::unique_ptr<FShaderCompileService> GShaderCompileService;
	}

	auto InitShaderCompileService() -> void
	{
		GShaderCompileService = std::make_unique<FShaderCompileService>();
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
			Output.ErrorMessage = "Shader compile service is not initialized";
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
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
		std::string& OutError) -> bool
	{
		if (!GShaderCompileService)
		{
			OutDependencies.clear();
			OutError = "Shader compile service is not initialized";
			return false;
		}
		return GShaderCompileService->BuildSourceDependencyManifest(
			VirtualShaderPath, Options, OutDependencies, OutError);
	}

	auto BuildShaderSourceTreeFingerprintFromService(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint,
		std::string& OutError) -> bool
	{
		if (!GShaderCompileService)
		{
			OutFingerprint = {};
			OutError = "Shader compile service is not initialized";
			return false;
		}
		return GShaderCompileService->BuildSourceTreeFingerprint(
			VirtualShaderPath, Options, OutFingerprint, OutError);
	}

	auto GetOrCompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput
	{
		if (!GShaderCompileService)
		{
			FShaderCompilerOutput Output;
			Output.ErrorMessage = "Shader compile service is not initialized";
			return Output;
		}
		return GShaderCompileService->GetOrCompileGenerated(Request);
	}

}
