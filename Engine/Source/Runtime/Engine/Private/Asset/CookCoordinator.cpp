#include "CookOutputInternal.h"
#include "CookDependencyDiscovery.h"
#include "Asset/OfflinePreparation.h"
#include "Asset/AssetCompilingManager.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "Asset/Cook.h"
#include "Shader/ShaderBuildProvider.h"

#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "Asset/References.h"
#include "Asset/MutationExtensions.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/References.h"
#include "DObject/Class.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "Engine/ProjectGameSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Project.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin
{
	using namespace AssetPrivate;
	namespace
	{
		const std::thread::id CookBootstrapOwner = std::this_thread::get_id();

		auto Failure(EAssetError Error, std::string Message) -> FAssetResult
		{
			return {Error, std::move(Message)};
		}

		struct FRegisteredCookContributor
		{
			FCookContributorHandle Handle = 0;
			FCookContributorRegistration Registration;

		};

		auto GetCookContributorMutex() -> std::mutex&
		{
			static std::mutex Mutex;
			return Mutex;
		}

		auto GetCookContributors()
			-> std::unordered_map<DClass*, std::shared_ptr<const FRegisteredCookContributor>>&
		{
			static std::unordered_map<DClass*, std::shared_ptr<const FRegisteredCookContributor>> Contributors;
			return Contributors;
		}

		auto GetNextCookContributorHandle() -> FCookContributorHandle&
		{
			static FCookContributorHandle Handle = 1;
			return Handle;
		}

		using FCookContributorSnapshot = std::unordered_map<
			DClass*, std::shared_ptr<const FRegisteredCookContributor>>;

		auto CaptureCookContributors() -> FCookContributorSnapshot
		{
			std::scoped_lock Lock(GetCookContributorMutex());
			return GetCookContributors();
		}

		auto ResolveCookContributor(DClass* Class,
			const FCookContributorSnapshot& Contributors,
			std::shared_ptr<const FRegisteredCookContributor>& OutContributor) -> FAssetResult
		{
			OutContributor.reset();
			for (DClass* Candidate = Class; Candidate; Candidate = Candidate->GetSuperClass())
			{
				const auto Found = Contributors.find(Candidate);
				if (Found == Contributors.end()) continue;
				OutContributor = Found->second;
				return {};
			}
			return Failure(EAssetError::UnsupportedProperty, std::format("CookUnsupportedClass: no contributor is registered for class {}.", Class ? Class->GetName() : "<null>"));
		}

		auto IsCancelled(const FCookCancellationCheck& Check) -> bool
		{
			return Check && Check();
		}

		auto ReadBoundedCookFile(const std::filesystem::path& Path, FByteBuffer& Out,
			const std::function<bool()>& Continue = {}) -> bool
		{
			Out.clear();
			auto File = FFileHelper::OpenRead(Path);
			if (!File || File->GetSize() > MaximumCookStateBytes) return false;
			Out.resize(static_cast<size_t>(File->GetSize()));
			constexpr size_t Chunk = 4 * 1024 * 1024;
			for (size_t Offset = 0; Offset < Out.size(); Offset += Chunk)
				if ((Continue && !Continue()) || !File->ReadAt(Offset,
					std::span(Out).subspan(Offset, std::min(Chunk, Out.size() - Offset))))
				{
					Out.clear(); return false;
				}
			return true;
		}

		auto ReadCachedCookPlan(const FCookRequest& Request, const FCookStateEntry& Entry,
			const std::function<bool()>& Continue, FCookSavePlan& OutPlan) -> bool
		{
			FByteBuffer PackageBytes, SegmentBytes;
			if (!ReadBoundedCookFile(Request.OutputRoot / RelativePackagePath(Entry.VirtualPackagePath), PackageBytes, Continue)
				|| (Entry.SegmentSize != 0 && !ReadBoundedCookFile(
					Request.OutputRoot / RelativeSegmentPath(Entry.VirtualPackagePath), SegmentBytes, Continue))
				|| PackageBytes.size() != Entry.PackageSize || SegmentBytes.size() != Entry.SegmentSize
				|| FXxHash128::HashBuffer(PackageBytes) != Entry.PackageDigest
				|| FXxHash128::HashBuffer(SegmentBytes) != Entry.SegmentDigest)
				return false;
			OutPlan = {.VirtualPath = Entry.VirtualPackagePath,
				.PackageBytes = std::move(PackageBytes), .BulkBytes = std::move(SegmentBytes),
				.BulkSummary = {Entry.SegmentSize, Entry.SegmentDigest},
				.InputFingerprint = Entry.InputFingerprint, .PackageDigest = Entry.PackageDigest,
				.SegmentDigest = Entry.SegmentDigest, .PackageFileSize = Entry.PackageSize,
				.SegmentFileSize = Entry.SegmentSize, .TargetPlatform = Request.TargetPlatform,
				.TargetProfile = Request.TargetProfile, .ContributorVersion = Entry.ContributorVersion,
				.FamilyProducerVersion = Entry.FamilyProducerVersion, .Contributor = Entry.Contributor,
				.BuildProvenance = Entry.BuildProvenance,
				.bRawBulkSegment = (Entry.SegmentFlags & CookStateSegmentRawFieldProjection) != 0,
				.bOpaqueRawSegment = (Entry.SegmentFlags & CookStateSegmentOpaque) != 0,
				.bReuseExistingOutput = true};
			return true;
		}

		auto MakeTopLevelObjectPath(
			const FTopLevelAssetPath& AssetPath, FObjectPath& OutPath) -> bool
		{
			return FObjectPath::TryCreate(
				AssetPath, std::span<const std::string>{}, OutPath);
		}

	} // namespace

	auto CookPackageStatusName(ECookPackageStatus Status) -> std::string_view
	{
		switch (Status)
		{
		case ECookPackageStatus::CookHit: return "cook-hit";
		case ECookPackageStatus::DdcHit: return "ddc-hit";
		case ECookPackageStatus::Rebuilt: return "rebuilt";
		case ECookPackageStatus::ReusedOutput: return "reused-output";
		case ECookPackageStatus::Captured: return "captured";
		case ECookPackageStatus::Failed: return "failed";
		case ECookPackageStatus::Cancelled: return "cancelled";
		case ECookPackageStatus::Unsupported: return "unsupported";
		}
		return "failed";
	}

	auto CookRunStatusName(ECookRunStatus Status) -> std::string_view
	{
		switch (Status)
		{
		case ECookRunStatus::Succeeded: return "succeeded";
		case ECookRunStatus::Failed: return "failed";
		case ECookRunStatus::Cancelled: return "cancelled";
		}
		return "failed";
	}

	auto CookOperationStageName(ECookOperationStage Stage) -> std::string_view
	{
		switch (Stage)
		{
		case ECookOperationStage::Discovery: return "discovery";
		case ECookOperationStage::Load: return "load";
		case ECookOperationStage::Prepare: return "prepare";
		case ECookOperationStage::Capture: return "capture";
		case ECookOperationStage::StageSegment: return "stage-segment";
		case ECookOperationStage::StagePackage: return "stage-package";
		case ECookOperationStage::StageAuxiliary: return "stage-auxiliary";
		case ECookOperationStage::CommitSegment: return "commit-segment";
		case ECookOperationStage::CommitPackage: return "commit-package";
		case ECookOperationStage::CommitAuxiliary: return "commit-auxiliary";
		case ECookOperationStage::CommitState: return "commit-state";
		case ECookOperationStage::CommitManifest: return "commit-manifest";
		case ECookOperationStage::Rollback: return "rollback";
		case ECookOperationStage::StaleCleanup: return "stale-cleanup";
		case ECookOperationStage::WriterLock: return "writer-lock";
		}
		return "discovery";
	}

	auto RegisterCookContributor(DClass* Class, FCookContributorRegistration Registration) -> FCookContributorHandle
	{
		if (!Class || Registration.Name.empty() || !Registration.Contribute
			|| Registration.ContributorVersion == 0
			|| Registration.FamilyProducerVersion == 0) return 0;
		auto Entry = std::make_shared<FRegisteredCookContributor>();
		Entry->Registration = Registration;
		std::scoped_lock Lock(GetCookContributorMutex());
		auto& Contributors = GetCookContributors();
		if (Contributors.contains(Class)) return 0;

		const FCookContributorHandle Handle = GetNextCookContributorHandle()++;
		Entry->Handle = Handle;
		Contributors.emplace(Class, std::move(Entry));
		return Handle;
	}

	auto UnregisterCookContributor(FCookContributorHandle Handle) -> void
	{
		if (Handle == 0) return;
		std::shared_ptr<const FRegisteredCookContributor> Retired;
		{
			std::scoped_lock Lock(GetCookContributorMutex());
			auto& Contributors = GetCookContributors();
			const auto Found = std::ranges::find_if(Contributors, [Handle](const auto& Pair) {
				return Pair.second->Handle == Handle;
			});
			if (Found == Contributors.end()) return;
			Retired = std::move(Found->second);
			Contributors.erase(Found);
		}
	}

	auto ValidateCookOutputRoot(const std::filesystem::path& OutputRoot,
		std::string& OutError) -> bool
	{
		if (OutputRoot.empty() || !OutputRoot.is_absolute())
			return CookFail("Cook output must be an absolute path.", &OutError);
		std::error_code Error;
		const auto Output = std::filesystem::weakly_canonical(OutputRoot, Error);
		if (Error) return CookFail(Error.message(), &OutError);
		std::vector<std::filesystem::path> Inputs;
		for (const auto& Mount : FMountPaths::GetRegisteredMountPoints())
		{
			Inputs.push_back(Mount.GetContentDir());
			for (const auto* Name : {"Shaders", "Source", "Config", "Configs"})
				Inputs.push_back(Mount.Root / Name);
		}
		for (const auto& Input : Inputs)
		{
			const auto Source = std::filesystem::weakly_canonical(Input, Error);
			if (Error) return CookFail(Error.message(), &OutError);
			std::filesystem::path Relative;
			if (FPaths::TryMakeLexicalRelativePath(Output, Source, Relative)
				|| FPaths::TryMakeLexicalRelativePath(Source, Output, Relative))
				return CookFail(std::format("Cook output overlaps an authored input tree: {}", Source.generic_string()), &OutError);
		}
		// Existing aliases anywhere in the writable tree must not redirect writes
		// outside that tree, including cache/log paths created before package work.
		if (std::filesystem::exists(Output, Error))
		{
			uint64 Entries = 0;
			std::filesystem::recursive_directory_iterator It(Output, Error), End;
			if (Error) return CookFail(Error.message(), &OutError);
			for (; It != End; It.increment(Error))
			{
				if (Error || ++Entries > MaximumCookStateEntries)
					return CookFail("Cook output tree is unreadable or exceeds its entry bound.", &OutError);
				if (!It->is_symlink(Error))
				{
					if (Error) return CookFail(Error.message(), &OutError);
					continue;
				}
				const auto Resolved = std::filesystem::weakly_canonical(It->path(), Error);
				std::filesystem::path Relative;
				if (Error || !FPaths::TryMakeLexicalRelativePath(Resolved, Output, Relative))
					return CookFail("Cook output contains an alias outside its writable tree.", &OutError);
			}
		}
		if (Error) return CookFail(Error.message(), &OutError);
		OutError.clear();
		return true;
	}

	auto FCookCoordinator::Run(const FCookRequest& Request, FCookRunResult& OutResult, ICookOutputStore* OutputStore, FCookFailureInjection ShouldFail) -> bool
	{
		const auto Start = std::chrono::steady_clock::now();
		OutResult = {};
		OutResult.TargetPlatform = Request.TargetPlatform;
		OutResult.TargetProfile = Request.TargetProfile;
		auto Finish = [&](ECookRunStatus Status, std::string Code,
						  std::string Diagnostic) -> bool {
			OutResult.Status = Status;
			if (Status == ECookRunStatus::Cancelled) OutResult.InputStatus = ECookInputStatus::Cancelled;
			OutResult.Code = std::move(Code);
			OutResult.Diagnostic = std::move(Diagnostic);
			OutResult.WallTimeNanoseconds = std::chrono::duration_cast<
												std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start)
												.count();
			return Status == ECookRunStatus::Succeeded;
		};
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game
			|| (!Request.bDryRun && (Request.OutputRoot.empty() || !Request.OutputRoot.is_absolute())))
			return Finish(ECookRunStatus::Failed, "invalid-request", "CookInvalidRequest: target/profile or output root is invalid.");
		std::string OutputError;
		if (!Request.OutputRoot.empty() && !ValidateCookOutputRoot(Request.OutputRoot, OutputError))
			return Finish(ECookRunStatus::Failed, "invalid-output-root", OutputError);
		if (GIsGameThreadIdInitialized ? !IsInGameThread() : std::this_thread::get_id() != CookBootstrapOwner)
			return Finish(ECookRunStatus::Failed, "wrong-thread", "Cook requires the object owner thread.");
		static std::atomic_flag Running = ATOMIC_FLAG_INIT;
		if (Running.test_and_set()) return Finish(ECookRunStatus::Failed, "cook-in-use", "A Cook run is already active.");
		struct FRunGuard { std::atomic_flag& Flag; ~FRunGuard() { Flag.clear(); } } RunGuard{Running};
		auto Contributors = CaptureCookContributors();
		if (IsCancelled(Request.IsCancelled))
			return Finish(ECookRunStatus::Cancelled, "cancelled", "CookCancelledBeforeDiscovery");

		std::vector<FPackagePath> Roots = Request.ExplicitRoots;
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			FProjectGameSettings Settings;
			const FProjectGameSettingsResult SettingsResult =
				FProjectGameSettingsStore::ForProject(*Project).Load(Settings);
			if (!SettingsResult)
				return Finish(ECookRunStatus::Failed, "project-settings-failed", std::format("CookProjectSettingsFailed: {}", SettingsResult.Message));
			if (!Settings.DefaultLevel.empty())
			{
				FPackagePath DefaultLevel;
				std::string PathError;
				if (!FPackagePath::TryCreate(Settings.DefaultLevel, DefaultLevel, &PathError))
					return Finish(ECookRunStatus::Failed, "invalid-default-level", std::format("CookInvalidDefaultLevel: {}: {}", Settings.DefaultLevel, PathError));
				Roots.push_back(std::move(DefaultLevel));
			}
		}
		std::ranges::sort(Roots, [](const FPackagePath& Left, const FPackagePath& Right) {
			return Left.GetView() < Right.GetView();
		});
		Roots.erase(std::unique(Roots.begin(), Roots.end()), Roots.end());
		FCookState PreviousState;
		bool bHasPreviousState = false;
		if (Request.IncrementalPolicy == ECookIncrementalPolicy::Enabled
			&& !Request.OutputRoot.empty())
		{
			FByteBuffer Bytes;
			bHasPreviousState = ReadBoundedCookFile(Request.OutputRoot / "CookState.bin", Bytes)
								&& DecodeCookState(Bytes, PreviousState)
								&& PreviousState.TargetPlatform == Request.TargetPlatform
								&& PreviousState.TargetProfile == Request.TargetProfile;
		}
		std::unordered_map<std::string, const FCookStateEntry*> PriorEntries;
		if (bHasPreviousState)
			for (const FCookStateEntry& Entry : PreviousState.Entries)
				PriorEntries.emplace(Entry.VirtualPackagePath, &Entry);

		std::vector<FCookSavePlan> Plans;
		FCookState NewState{Request.TargetPlatform, Request.TargetProfile};
		std::vector<FCookAuxiliaryOutput> AuxiliaryOutputs;
		uint64 RetainedOutputBytes = 0;
		FAssetCompilingManager::Get().FinishAllCompilation();
		{
			FScopedOfflinePreparation Preparation;
			FAssetPackageLoadScope LoadScope;
			struct FReleaseLoads
			{
				FAssetPackageLoadScope& Scope;
				~FReleaseLoads()
				{
					FAssetCompilingManager::Get().FinishAllCompilation();
					(void)Scope.Release();
				}
			} ReleaseLoads{LoadScope};
			AssetPrivate::FCookDependencyDiscovery Inputs(Request, CaptureAssetRegistrySnapshot(),
				[&](const FAssetData& Data, FCookContributorRegistration& Out) -> FAssetResult {
					if (Data.TopLevelAssets.empty()) return Failure(EAssetError::InvalidPackageType, "Cook package has no assets.");
					std::shared_ptr<const FRegisteredCookContributor> Entry;
					const auto Result = ResolveCookContributor(FindClassByQualifiedName(FName(
						Data.TopLevelAssets.front().AssetClassName)), Contributors, Entry);
					if (Result) Out = Entry->Registration;
					return Result;
				});
			auto CheckInputs = [&]() -> FAssetResult { return Inputs.CheckCancellation(); };
			auto RetainOutput = [&](uint64 PackageBytes, uint64 BulkBytes) -> bool {
				constexpr uint64 MaximumOutputBytes = 1024ull * 1024 * 1024;
				if (PackageBytes > MaximumOutputBytes - RetainedOutputBytes
					|| BulkBytes > MaximumOutputBytes - RetainedOutputBytes - PackageBytes)
				{
					OutResult.InputStatus = ECookInputStatus::LimitExceeded;
					OutResult.InputFailure = {EAssetError::CorruptFile, "Cook detached output byte limit exceeded."};
					return false;
				}
				RetainedOutputBytes += PackageBytes + BulkBytes;
				OutResult.PeakRetainedBytes = std::max(OutResult.PeakRetainedBytes, RetainedOutputBytes + Inputs.GetRetainedBytes());
				return true;
			};
			auto InputFailure = [&](const FAssetResult& Result) -> bool {
				OutResult.InputFailure = Result;
				OutResult.InputStatus = Inputs.GetStatus() == ECookInputStatus::None
					? ECookInputStatus::InvalidDependency : Inputs.GetStatus();
				return Finish(Inputs.GetStatus() == ECookInputStatus::Cancelled
					? ECookRunStatus::Cancelled : ECookRunStatus::Failed, "input-failed", Result.Message);
			};
			bool bPrepared = false;
			try
			{
				bPrepared = [&]() -> bool {
					FAssetReferenceStoreCapture ExternalRoots;
					if (auto Result = CaptureAssetReferenceStores(ExternalRoots); !Result) return InputFailure(Result);
					if (auto Result = Inputs.Acquire(Roots, ExternalRoots); !Result) return InputFailure(Result);
					const auto& Packages = Inputs.GetPackages();
					const auto& Catalog = Inputs.GetRegistry().Catalog;
					for (size_t Index = 0; Index < Packages.size(); ++Index)
					{
						if (IsCancelled(Request.IsCancelled))
							return Finish(ECookRunStatus::Cancelled, "cancelled", "CookCancelledBeforePackagePreparation");
						const FPackagePath& Path = Packages[Index];
						if (Request.ReportProgress) Request.ReportProgress({ECookOperationStage::Load, Path, Index, Packages.size()});
						if (ShouldFail && ShouldFail(ECookOperationStage::Load, Index, OutResult.Diagnostic))
							return Finish(ECookRunStatus::Failed, "load-injected-failure", OutResult.Diagnostic);
						if (auto Result = CheckInputs(); !Result) return InputFailure(Result);
						const FAssetData* Data = Catalog.FindExact(Path);
						if (!Data) return Finish(ECookRunStatus::Failed, "stale-registry", std::format("CookStaleRegistry: {} disappeared from the captured catalog.", Path.ToString()));
						if (Data->TopLevelAssets.empty())
							return Finish(ECookRunStatus::Failed, "missing-top-level-asset",
								std::format("CookMissingTopLevelAsset: {} has no independently addressable asset.",
									Path.ToString()));
						const FTopLevelAssetData& CookRoot = Data->TopLevelAssets.front();
						const auto& Contributor = Inputs.GetContributor(Path);
						const auto& Dependencies = Inputs.GetDependencies(Path);
						FXxHash128 Fingerprint;
						std::string Error;
						if (!FingerprintCookBuildDependencies(Dependencies, Fingerprint, &Error))
							return Finish(ECookRunStatus::Failed, "fingerprint-failed", Error);
						const auto Prior = PriorEntries.find(Path.ToString());
						bool bCookHit = Inputs.IsReusable(Path)
										&& Prior != PriorEntries.end()
										&& Prior->second->InputFingerprint == Fingerprint
										&& Prior->second->ContributorVersion
											   == Contributor.ContributorVersion
										&& Prior->second->FamilyProducerVersion
											   == Contributor.FamilyProducerVersion
										&& Prior->second->BuildDependencies == Dependencies;
						FCookSavePlan CachedPlan;
						if (bCookHit)
							bCookHit = ReadCachedCookPlan(Request, *Prior->second,
								[&] { return CheckInputs().Succeeded(); }, CachedPlan);
						if (auto Result = CheckInputs(); !Result) return InputFailure(Result);
						if (bCookHit)
						{
							const FCookStateEntry& Hit = *Prior->second;
							if (!RetainOutput(Hit.PackageSize, Hit.SegmentSize)) return Finish(ECookRunStatus::Failed, "output-limit", OutResult.InputFailure.Message);
							Plans.push_back(std::move(CachedPlan));
							NewState.Entries.push_back(Hit);
							OutResult.ReusedBytes += Hit.PackageSize + Hit.SegmentSize;
							OutResult.Packages.push_back({{}, Path, Hit.Contributor, "cook-hit", "Validated unchanged Cook outputs.", ECookPackageStatus::CookHit, ECookOperationStage::Capture, Hit.PackageSize, Hit.SegmentSize});
							continue;
						}

						FObjectPath CookRootPath;
						if (!MakeTopLevelObjectPath(CookRoot.AssetPath, CookRootPath))
							return Finish(ECookRunStatus::Failed, "invalid-top-level-asset",
								std::format("CookInvalidTopLevelAsset: {}.",
									CookRoot.AssetPath.ToString()));
						DObject* Asset = nullptr;
						const FAssetResult LoadResult = LoadScope.LoadObject(CookRootPath, Asset);
						if (!LoadResult || !Asset) return InputFailure(LoadResult);
						FCookContext Context(Request.TargetPlatform, Request.TargetProfile, Request.bRetainEditorOnlyData);
						Context.SetInputReader([&](auto Kind, auto Name, FByteBuffer& Bytes) {
							return Inputs.ReadInput(Path, Kind, Name, Bytes);
						});

						if (ShouldFail && ShouldFail(ECookOperationStage::Prepare, Index, Error))
							return Finish(ECookRunStatus::Failed, "prepare-injected-failure", Error);
						DPackage* AuthoredPackage = Asset->GetPackage();
						if (!AuthoredPackage)
							return Finish(ECookRunStatus::Failed, "missing-package", std::format("CookMissingPackage: package={}, contributor={}", Path.ToString(), Contributor.Name));
						const FAssetResult Contribution = Contributor.Contribute(
							*Asset, Path.GetView(), Context
						);
						if (auto Result = CheckInputs(); !Result) return InputFailure(Result);
						if (!Contribution)
							return Finish(ECookRunStatus::Failed, "contribution-failed", std::format("CookContributionFailed: package={}, contributor={}, stage=prepare: {}", Path.ToString(), Contributor.Name, Contribution.Message));
						if (ShouldFail && ShouldFail(ECookOperationStage::Capture, Index, Error))
							return Finish(ECookRunStatus::Failed, "capture-injected-failure", Error);
						std::vector<FCookSavePlan> PackagePlans;
						if (!Context.TakeSavePlans(PackagePlans, &Error) || PackagePlans.size() != 1)
							return Finish(ECookRunStatus::Failed, "capture-failed", std::format("CookCaptureFailed: package={}, contributor={}: {}", Path.ToString(), Contributor.Name, Error));
						FCookSavePlan Plan = std::move(PackagePlans.front());
						Plan.InputFingerprint = Fingerprint;
						Plan.Contributor = Contributor.Name;
						Plan.ContributorVersion = Contributor.ContributorVersion;
						Plan.FamilyProducerVersion = Contributor.FamilyProducerVersion;
						const ECookPackageStatus PreparationStatus =
							Contributor.ClassifyPreparation ? Contributor.ClassifyPreparation(*Asset) : ECookPackageStatus::Captured;
						Plan.BuildProvenance = CookPackageStatusName(PreparationStatus);
						if (!RetainOutput(Plan.PackageFileSize, Plan.SegmentFileSize)) return Finish(ECookRunStatus::Failed, "output-limit", OutResult.InputFailure.Message);
						OutResult.ChangedBytes += Plan.PackageFileSize + Plan.SegmentFileSize;
						NewState.Entries.push_back(MakeCookStateEntry(Plan));
						OutResult.Packages.push_back({{}, Path, Plan.Contributor, std::string(CookPackageStatusName(PreparationStatus)), "Captured deterministic package save plan.", PreparationStatus, ECookOperationStage::Capture, Plan.PackageFileSize, Plan.SegmentFileSize});
						NewState.Entries.back().BuildDependencies = Dependencies;
						if (auto Result = CheckInputs(); !Result) return InputFailure(Result);
						Plans.push_back(std::move(Plan));
					}
					if (Request.ReportProgress) Request.ReportProgress({ECookOperationStage::StageAuxiliary, {}, Packages.size(), Packages.size()});
					std::string Error;
					if (ShouldFail && ShouldFail(ECookOperationStage::StageAuxiliary, 0, Error))
						return Finish(ECookRunStatus::Failed, "auxiliary-injected-failure", Error);
					FByteBuffer ShaderBytes;
					if (!BuildCookedShaderLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, ShaderBytes, Error, Request.IsCancelled))
						return IsCancelled(Request.IsCancelled)
							? Finish(ECookRunStatus::Cancelled, "cancelled", Error)
							: Finish(ECookRunStatus::Failed, "shader-library-failed", Error);
					if (!RetainOutput(ShaderBytes.size(), 0)) return Finish(ECookRunStatus::Failed, "output-limit", OutResult.InputFailure.Message);
					AuxiliaryOutputs.push_back({ECookManifestEntryKind::ShaderLibrary,
						std::string(ShaderCookedLibraryRelativePath), std::move(ShaderBytes)});
					AuxiliaryOutputs.back().Digest = FXxHash128::HashBuffer(AuxiliaryOutputs.back().Bytes);
					if (auto Result = CheckInputs(); !Result) return InputFailure(Result);
					OutResult.PeakRetainedBytes = std::max(OutResult.PeakRetainedBytes,
						RetainedOutputBytes + Inputs.GetRetainedBytes());
					return true;
				}();
			}
			catch (const std::exception& Error)
			{
				return InputFailure({EAssetError::InUse, Error.what()});
			}
			if (!bPrepared) return false;
		}
		Contributors.clear();
		std::ranges::sort(Plans, {}, &FCookSavePlan::VirtualPath);
		std::ranges::sort(NewState.Entries, {}, &FCookStateEntry::VirtualPackagePath);
		if (Request.bDryRun) return Finish(ECookRunStatus::Succeeded, "dry-run", "Cook dry-run captured package and auxiliary outputs.");
		std::unique_ptr<ICookOutputStore> OwnedStore;
		if (!OutputStore)
		{
			OwnedStore = CreateLocalLooseCookOutputStore(Request.OutputRoot, Request.TargetPlatform, Request.TargetProfile);
			OutputStore = OwnedStore.get();
		}
		FCookPublishResult PublishResult = OutputStore->Publish(
			Plans, AuxiliaryOutputs, NewState, OutResult, Request.IsCancelled, ShouldFail);
		if (!PublishResult)
		{
			if (PublishResult.Status == ECookPublishStatus::Cancelled)
				return Finish(ECookRunStatus::Cancelled, "cancelled",
					std::move(PublishResult.Diagnostic));
			return Finish(ECookRunStatus::Failed, "publication-failed",
				std::move(PublishResult.Diagnostic));
		}
		return Finish(ECookRunStatus::Succeeded, "succeeded", "Cook published a validated manifest-last output generation.");
	}
} // namespace Durin
