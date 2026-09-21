#include "Misc/PackageWriter.h"
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
			std::shared_ptr<const FRegisteredCookContributor>& OutContributor) -> FCookContributionResult
		{
			OutContributor.reset();
			for (DClass* Candidate = Class; Candidate; Candidate = Candidate->GetSuperClass())
			{
				const auto Found = Contributors.find(Candidate);
				if (Found == Contributors.end()) continue;
				OutContributor = Found->second;
				return {};
			}
			return {.Error = ECookContributionError::UnsupportedClass,
				.ObjectPath = Class ? Class->GetQualifiedName().ToString() : "<null>"};
		}

		auto IsCancelled(const FCookCancellationCheck& Check) -> bool
		{
			return Check && Check();
		}

		auto ReadBoundedCookFile(const std::filesystem::path& Path, FByteBuffer& Out,
			const std::function<bool()>& Continue = {}) -> bool
		{
			const std::array Paths{Path};
			auto Access = FPackageFileAccess::TryAcquire(Paths, false);
			if (!Access) return false;
			Out.clear();
			auto File = FFileHelper::OpenRead(Path);
			if (!File || (*File)->GetSize() > MaximumCookStateBytes) return false;
			Out.resize(static_cast<size_t>((*File)->GetSize()));
			constexpr size_t Chunk = 4 * 1024 * 1024;
			for (size_t Offset = 0; Offset < Out.size(); Offset += Chunk)
				if ((Continue && !Continue()) || !(*File)->ReadAt(Offset,
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

	auto FormatCookPackageResult(const FCookPackageResult& Result) -> std::string
	{
		return Result.Status == ECookPackageStatus::CookHit
			? "Validated unchanged Cook outputs." : "Captured deterministic package save plan.";
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

	auto FormatCookContributorRegistrationError(const FCookContributorRegistrationResult& Result) -> std::string
	{
		if (Result) return {};
		return std::format("Cook contributor registration failed ({}): contributor={}, class={}",
			static_cast<uint8>(Result.Error), Result.ContributorName, Result.ClassName);
	}

	auto RegisterCookContributor(DClass* Class, FCookContributorRegistration Registration) -> FCookContributorRegistrationResult
	{
		auto Reject = [&](ECookContributorRegistrationError Error) -> FCookContributorRegistrationResult {
			return {.Error = Error, .ContributorName = Registration.Name,
				.ClassName = Class ? Class->GetQualifiedName().ToString() : std::string{},
				.ContributorVersion = Registration.ContributorVersion, .FamilyProducerVersion = Registration.FamilyProducerVersion};
		};
		if (!Class) return Reject(ECookContributorRegistrationError::InvalidClass);
		if (Registration.Name.empty()) return Reject(ECookContributorRegistrationError::Name);
		if (!Registration.Contribute) return Reject(ECookContributorRegistrationError::Callback);
		if (Registration.ContributorVersion == 0 || Registration.FamilyProducerVersion == 0)
			return Reject(ECookContributorRegistrationError::Version);
		auto Entry = std::make_shared<FRegisteredCookContributor>();
		Entry->Registration = Registration;
		std::scoped_lock Lock(GetCookContributorMutex());
		auto& Contributors = GetCookContributors();
		if (Contributors.contains(Class)) return Reject(ECookContributorRegistrationError::DuplicateClass);

		const FCookContributorHandle Handle = GetNextCookContributorHandle()++;
		Entry->Handle = Handle;
		Contributors.emplace(Class, std::move(Entry));
		return {.Handle = Handle};
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

	auto FormatCookOutputRootError(const FCookOutputRootResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case ECookOutputRootError::None: return {};
		case ECookOutputRootError::AbsolutePath: return "Cook output must be an absolute path.";
		case ECookOutputRootError::AuthoredOverlap: return "Cook output overlaps an authored input tree: " + Result.RelatedPath.generic_string();
		case ECookOutputRootError::EntryLimit: return std::format("Cook output tree exceeds its {} entry bound.", Result.MaximumEntries);
		case ECookOutputRootError::AliasEscape: return "Cook output contains an alias outside its writable tree: " + Result.RelatedPath.generic_string();
		default: return Result.SystemError.message();
		}
	}

	auto ValidateCookOutputRoot(const std::filesystem::path& OutputRoot) -> FCookOutputRootResult
	{
		if (OutputRoot.empty() || !OutputRoot.is_absolute())
			return {.Error = ECookOutputRootError::AbsolutePath, .OutputRoot = OutputRoot};
		std::error_code Error;
		const auto Output = std::filesystem::weakly_canonical(OutputRoot, Error);
		if (Error) return {.Error = ECookOutputRootError::CanonicalizeOutput, .OutputRoot = OutputRoot, .SystemError = Error};
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
			if (Error) return {.Error = ECookOutputRootError::CanonicalizeInput, .OutputRoot = OutputRoot, .RelatedPath = Input, .SystemError = Error};
			std::filesystem::path Relative;
			if (FPaths::TryMakeLexicalRelativePath(Output, Source, Relative)
				|| FPaths::TryMakeLexicalRelativePath(Source, Output, Relative))
				return {.Error = ECookOutputRootError::AuthoredOverlap, .OutputRoot = OutputRoot, .RelatedPath = Source};
		}
		// Existing aliases anywhere in the writable tree must not redirect writes
		// outside that tree, including cache/log paths created before package work.
		if (std::filesystem::exists(Output, Error))
		{
			uint64 Entries = 0;
			std::filesystem::recursive_directory_iterator It(Output, Error), End;
			if (Error) return {.Error = ECookOutputRootError::EnumerateTree, .OutputRoot = OutputRoot, .RelatedPath = Output, .SystemError = Error};
			for (; It != End; It.increment(Error))
			{
				if (Error)
					return {.Error = ECookOutputRootError::EnumerateTree, .OutputRoot = OutputRoot, .SystemError = Error, .Entries = Entries};
				if (++Entries > MaximumCookStateEntries)
					return {.Error = ECookOutputRootError::EntryLimit, .OutputRoot = OutputRoot, .Entries = Entries, .MaximumEntries = MaximumCookStateEntries};
				if (!It->is_symlink(Error))
				{
					if (Error) return {.Error = ECookOutputRootError::InspectEntry, .OutputRoot = OutputRoot, .RelatedPath = It->path(), .SystemError = Error};
					continue;
				}
				const auto Resolved = std::filesystem::weakly_canonical(It->path(), Error);
				std::filesystem::path Relative;
				if (Error)
					return {.Error = ECookOutputRootError::ResolveAlias, .OutputRoot = OutputRoot, .RelatedPath = It->path(), .SystemError = Error};
				if (!FPaths::TryMakeLexicalRelativePath(Resolved, Output, Relative))
					return {.Error = ECookOutputRootError::AliasEscape, .OutputRoot = OutputRoot, .RelatedPath = It->path(), .ResolvedPath = Resolved};
			}
			if (Error) return {.Error = ECookOutputRootError::EnumerateTree, .OutputRoot = OutputRoot, .SystemError = Error, .Entries = Entries};
		}
		if (Error) return {.Error = ECookOutputRootError::InspectTree, .OutputRoot = OutputRoot, .SystemError = Error};
		return {};
	}

	auto CookRunCodeName(const FCookRunResult& Result) -> std::string_view
	{
		switch (Result.Error)
		{
		case ECookRunError::None: return Result.bDryRun ? "dry-run" : "succeeded";
		case ECookRunError::Unspecified: return "unspecified";
		case ECookRunError::InvalidRequest: return "invalid-request";
		case ECookRunError::InvalidOutputRoot: return "invalid-output-root";
		case ECookRunError::WrongThread: return "wrong-thread";
		case ECookRunError::CookInUse: return "cook-in-use";
		case ECookRunError::Cancelled: return "cancelled";
		case ECookRunError::ProjectSettingsFailed: return "project-settings-failed";
		case ECookRunError::InvalidDefaultLevel: return "invalid-default-level";
		case ECookRunError::InputFailed: return "input-failed";
		case ECookRunError::LoadInjectedFailure: return "load-injected-failure";
		case ECookRunError::StaleRegistry: return "stale-registry";
		case ECookRunError::MissingTopLevelAsset: return "missing-top-level-asset";
		case ECookRunError::FingerprintFailed: return "fingerprint-failed";
		case ECookRunError::OutputLimit: return "output-limit";
		case ECookRunError::InvalidTopLevelAsset: return "invalid-top-level-asset";
		case ECookRunError::PrepareInjectedFailure: return "prepare-injected-failure";
		case ECookRunError::MissingPackage: return "missing-package";
		case ECookRunError::ContributionFailed: return "contribution-failed";
		case ECookRunError::CaptureInjectedFailure: return "capture-injected-failure";
		case ECookRunError::CaptureFailed: return "capture-failed";
		case ECookRunError::AuxiliaryInjectedFailure: return "auxiliary-injected-failure";
		case ECookRunError::ShaderLibraryFailed: return "shader-library-failed";
		case ECookRunError::PublicationFailed: return "publication-failed";
		}
		return "unknown";
	}

	auto FormatCookCaptureError(const FCookCaptureResult& Result) -> std::string
	{
		if (Result) return {};
		const auto Detail = Result.Error == ECookCaptureError::PlanCount
			? std::format("Expected {} Cook save plan, got {}.", Result.ExpectedPlans, Result.ActualPlans)
			: Result.PlanCause ? FormatCookPlanError(*Result.PlanCause) : "Cook plan finalization failed.";
		return std::format("CookCaptureFailed: package={}, contributor={}: {}",
			Result.Package.ToString(), Result.Contributor, Detail);
	}

	auto FormatCookRunError(const FCookRunResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case ECookRunError::None: return Result.bDryRun ? "Cook dry-run captured package and auxiliary outputs." : "Cook published a validated manifest-last output generation.";
		case ECookRunError::Unspecified: return "Cook run has no terminal result.";
		case ECookRunError::InvalidRequest: return "CookInvalidRequest: target/profile or output root is invalid.";
		case ECookRunError::InvalidOutputRoot: return Result.OutputRootCause ? FormatCookOutputRootError(*Result.OutputRootCause) : "Cook output root is invalid.";
		case ECookRunError::WrongThread: return "Cook requires the object owner thread.";
		case ECookRunError::CookInUse: return "A Cook run is already active.";
		case ECookRunError::Cancelled:
			if (Result.PublicationCause) return FormatCookPublishError(*Result.PublicationCause);
			if (Result.ShaderCause) return FormatShaderError(*Result.ShaderCause);
			return Result.Stage == ECookOperationStage::Discovery ? "CookCancelledBeforeDiscovery" : "CookCancelledBeforePackagePreparation";
		case ECookRunError::ProjectSettingsFailed: return Result.SettingsCause ? std::format("CookProjectSettingsFailed: {}", Result.SettingsCause->Message) : "Cook project settings failed.";
		case ECookRunError::InvalidDefaultLevel: return std::format("CookInvalidDefaultLevel: {}: {}", Result.AssetIdentity, Result.DefaultLevelCause ? ToString(*Result.DefaultLevelCause) : "Invalid path");
		case ECookRunError::InputFailed: return Result.InputFailure.ToString();
		case ECookRunError::LoadInjectedFailure:
		case ECookRunError::PrepareInjectedFailure:
		case ECookRunError::CaptureInjectedFailure:
		case ECookRunError::AuxiliaryInjectedFailure: return Result.InjectionCause ? Result.InjectionCause->ExternalDiagnostic : "Cook injected failure.";
		case ECookRunError::StaleRegistry: return std::format("CookStaleRegistry: {} disappeared from the captured catalog.", Result.CurrentPackage.ToString());
		case ECookRunError::MissingTopLevelAsset: return std::format("CookMissingTopLevelAsset: {} has no independently addressable asset.", Result.CurrentPackage.ToString());
		case ECookRunError::FingerprintFailed: return Result.FingerprintCause ? FormatCookDependencyCodecError(*Result.FingerprintCause) : "Cook fingerprint failed.";
		case ECookRunError::OutputLimit: return std::format("Cook detached output byte limit exceeded: retained={}, package={}, bulk={}, maximum={}.", Result.RetainedOutputBytes, Result.RequestedPackageBytes, Result.RequestedBulkBytes, Result.MaximumOutputBytes);
		case ECookRunError::InvalidTopLevelAsset: return std::format("CookInvalidTopLevelAsset: {}.", Result.AssetIdentity);
		case ECookRunError::MissingPackage: return std::format("CookMissingPackage: package={}, contributor={}", Result.CurrentPackage.ToString(), Result.CurrentContributor);
		case ECookRunError::ContributionFailed: return std::format("CookContributionFailed: package={}, contributor={}, stage=prepare: {}", Result.ContributionPackage.ToString(), Result.ContributionProvider, Result.ContributionCause ? FormatCookContributionError(*Result.ContributionCause) : "Contribution failed");
		case ECookRunError::CaptureFailed: return Result.CaptureCause ? FormatCookCaptureError(*Result.CaptureCause) : "Cook capture failed.";
		case ECookRunError::ShaderLibraryFailed: return Result.ShaderCause ? FormatShaderError(*Result.ShaderCause) : "Cook shader library failed.";
		case ECookRunError::PublicationFailed: return Result.PublicationCause ? FormatCookPublishError(*Result.PublicationCause) : "Cook publication failed.";
		}
		return "Unknown Cook failure.";
	}

	auto FCookCoordinator::Run(const FCookRequest& Request, FCookRunResult& OutResult, ICookOutputStore* OutputStore, FCookFailureInjection ShouldFail) -> bool
	{
		const auto Start = std::chrono::steady_clock::now();
		OutResult = {};
		OutResult.bDryRun = Request.bDryRun;
		OutResult.OutputRoot = Request.OutputRoot;
		OutResult.TargetPlatform = Request.TargetPlatform;
		OutResult.TargetProfile = Request.TargetProfile;
		auto Finish = [&](ECookRunStatus Status, ECookRunError Error) -> bool {
			OutResult.Status = Status;
			if (Status == ECookRunStatus::Cancelled) OutResult.InputFailure.Status = ECookInputStatus::Cancelled;
			OutResult.Error = Error;
			OutResult.WallTimeNanoseconds = std::chrono::duration_cast<
												std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start)
												.count();
			return static_cast<bool>(OutResult);
		};
		auto Injected = [&](ECookOperationStage Stage, size_t Index) -> bool {
			OutResult.Stage = Stage;
			std::string ExternalDiagnostic;
			if (!ShouldFail || !ShouldFail(Stage, Index, ExternalDiagnostic)) return false;
			if (ExternalDiagnostic.size() > 2048) ExternalDiagnostic.resize(2048);
			OutResult.InjectionCause = FCookRunInjectionCause{Stage, Index, std::move(ExternalDiagnostic)};
			return true;
		};
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game
			|| (!Request.bDryRun && (Request.OutputRoot.empty() || !Request.OutputRoot.is_absolute())))
			return Finish(ECookRunStatus::Failed, ECookRunError::InvalidRequest);
		if (!Request.OutputRoot.empty())
			if (const auto Validated = ValidateCookOutputRoot(Request.OutputRoot); !Validated)
			{
				OutResult.OutputRootCause = std::make_shared<FCookOutputRootResult>(Validated);
				return Finish(ECookRunStatus::Failed, ECookRunError::InvalidOutputRoot);
			}
		if (GIsGameThreadIdInitialized ? !IsInGameThread() : std::this_thread::get_id() != CookBootstrapOwner)
			return Finish(ECookRunStatus::Failed, ECookRunError::WrongThread);
		static std::atomic_flag Running = ATOMIC_FLAG_INIT;
		if (Running.test_and_set()) return Finish(ECookRunStatus::Failed, ECookRunError::CookInUse);
		struct FRunGuard { std::atomic_flag& Flag; ~FRunGuard() { Flag.clear(); } } RunGuard{Running};
		auto Contributors = CaptureCookContributors();
		if (IsCancelled(Request.IsCancelled))
			return Finish(ECookRunStatus::Cancelled, ECookRunError::Cancelled);

		std::vector<FPackagePath> Roots = Request.ExplicitRoots;
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			FProjectGameSettings Settings;
			const FProjectGameSettingsResult SettingsResult =
				FProjectGameSettingsStore::ForProject(*Project).Load(Settings);
			if (!SettingsResult)
			{
				OutResult.SettingsCause = std::make_shared<FProjectGameSettingsResult>(SettingsResult);
				return Finish(ECookRunStatus::Failed, ECookRunError::ProjectSettingsFailed);
			}
			if (!Settings.DefaultLevel.empty())
			{
				FPackagePath DefaultLevel;
				if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(Settings.DefaultLevel, DefaultLevel); !PathValidation)
				{
					OutResult.AssetIdentity = Settings.DefaultLevel;
					OutResult.DefaultLevelCause = std::make_shared<FObjectPathError>(PathValidation.error());
					return Finish(ECookRunStatus::Failed, ECookRunError::InvalidDefaultLevel);
				}
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
				[&](const FAssetData& Data, FCookContributorRegistration& Out) -> FCookContributionResult {
					if (Data.TopLevelAssets.empty()) return {.Error = ECookContributionError::Plan,
						.PlanCause = FCookPlanError{.Code = ECookPlanError::EmptyPackage, .VirtualPath = Data.PackagePath.ToString()}};
					std::shared_ptr<const FRegisteredCookContributor> Entry;
					const auto Result = ResolveCookContributor(FindClassByQualifiedName(FName(
						Data.TopLevelAssets.front().AssetClassName)), Contributors, Entry);
					if (Result) Out = Entry->Registration;
					return Result;
				});
			auto CheckInputs = [&]() -> FCookInputResult { return Inputs.CheckCancellation(); };
			auto RetainOutput = [&](uint64 PackageBytes, uint64 BulkBytes) -> bool {
				constexpr uint64 MaximumOutputBytes = 1024ull * 1024 * 1024;
				if (PackageBytes > MaximumOutputBytes - RetainedOutputBytes
					|| BulkBytes > MaximumOutputBytes - RetainedOutputBytes - PackageBytes)
				{
					OutResult.InputFailure.Status = ECookInputStatus::LimitExceeded;

					OutResult.RetainedOutputBytes = RetainedOutputBytes;
					OutResult.RequestedPackageBytes = PackageBytes;
					OutResult.RequestedBulkBytes = BulkBytes;
					OutResult.MaximumOutputBytes = MaximumOutputBytes;
					return false;
				}
				RetainedOutputBytes += PackageBytes + BulkBytes;
				OutResult.PeakRetainedBytes = std::max(OutResult.PeakRetainedBytes, RetainedOutputBytes + Inputs.GetRetainedBytes());
				return true;
			};
			auto InputFailure = [&](const FCookInputResult& Result) -> bool {
				OutResult.InputFailure = Result;

				return Finish(Result.Status == ECookInputStatus::Cancelled
					? ECookRunStatus::Cancelled : ECookRunStatus::Failed, ECookRunError::InputFailed);
			};
			bool bPrepared = false;
			try
			{
				bPrepared = [&]() -> bool {
					FAssetReferenceStoreCapture ExternalRoots;
					if (auto Result = CaptureAssetReferenceStores(ExternalRoots); !Result) return InputFailure(ToCookInputResult(Result));
					if (auto Result = Inputs.Acquire(Roots, ExternalRoots); !Result) return InputFailure(Result);
					const auto& Packages = Inputs.GetPackages();
					const auto& Catalog = Inputs.GetRegistry().Catalog;
					for (size_t Index = 0; Index < Packages.size(); ++Index)
					{
						OutResult.Stage = ECookOperationStage::Prepare;
						if (IsCancelled(Request.IsCancelled))
							return Finish(ECookRunStatus::Cancelled, ECookRunError::Cancelled);
						const FPackagePath& Path = Packages[Index];
						OutResult.CurrentPackage = Path;
						OutResult.Stage = ECookOperationStage::Load;
						if (Request.ReportProgress) Request.ReportProgress({ECookOperationStage::Load, Path, Index, Packages.size()});
						if (Injected(ECookOperationStage::Load, Index))
							return Finish(ECookRunStatus::Failed, ECookRunError::LoadInjectedFailure);
						if (auto Result = CheckInputs(); !Result) return InputFailure(Result);
						const FAssetData* Data = Catalog.FindExact(Path);
						if (!Data) return Finish(ECookRunStatus::Failed, ECookRunError::StaleRegistry);
						if (Data->TopLevelAssets.empty())
							return Finish(ECookRunStatus::Failed, ECookRunError::MissingTopLevelAsset);
						const FTopLevelAssetData& CookRoot = Data->TopLevelAssets.front();
						const auto& Contributor = Inputs.GetContributor(Path);
						OutResult.CurrentContributor = Contributor.Name;
						OutResult.AssetIdentity = CookRoot.AssetPath.ToString();
						const auto& Dependencies = Inputs.GetDependencies(Path);
						FXxHash128 Fingerprint;
						if (const auto Fingerprinted = FingerprintCookBuildDependencies(Dependencies, Fingerprint); !Fingerprinted)
						{
							OutResult.FingerprintCause = Fingerprinted;
							return Finish(ECookRunStatus::Failed, ECookRunError::FingerprintFailed);
						}
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
							if (!RetainOutput(Hit.PackageSize, Hit.SegmentSize)) return Finish(ECookRunStatus::Failed, ECookRunError::OutputLimit);
							Plans.push_back(std::move(CachedPlan));
							NewState.Entries.push_back(Hit);
							OutResult.ReusedBytes += Hit.PackageSize + Hit.SegmentSize;
							OutResult.Packages.push_back({{}, Path, Hit.Contributor, ECookPackageStatus::CookHit, ECookOperationStage::Capture, Hit.PackageSize, Hit.SegmentSize});
							continue;
						}

						FObjectPath CookRootPath;
						if (!MakeTopLevelObjectPath(CookRoot.AssetPath, CookRootPath))
							return Finish(ECookRunStatus::Failed, ECookRunError::InvalidTopLevelAsset);
						DObject* Asset = nullptr;
						const auto LoadResult = LoadScope.LoadObject<DObject>(CookRootPath);
						Asset = LoadResult.value_or(nullptr);
						if (!LoadResult) return InputFailure(ToCookInputResult(AssetReadResultFromError(LoadResult.error())));
						if (!Asset) return InputFailure({ECookInputStatus::InvalidDependency, "Cook load returned no object."});
						FCookContext Context(Request.TargetPlatform, Request.TargetProfile, Request.bRetainEditorOnlyData);
						Context.SetInputReader([&](auto Kind, auto Name, FByteBuffer& Bytes) {
							return Inputs.ReadInput(Path, Kind, Name, Bytes);
						});

						if (Injected(ECookOperationStage::Prepare, Index))
							return Finish(ECookRunStatus::Failed, ECookRunError::PrepareInjectedFailure);
						DPackage* AuthoredPackage = Asset->GetPackage();
						if (!AuthoredPackage)
							return Finish(ECookRunStatus::Failed, ECookRunError::MissingPackage);
						const FCookContributionResult Contribution = Contributor.Contribute(
							*Asset, Path.GetView(), Context
						);
						if (auto Result = CheckInputs(); !Result) return InputFailure(Result);
						if (!Contribution)
						{
							OutResult.ContributionCause = std::make_shared<FCookContributionResult>(Contribution);
							OutResult.ContributionPackage = Path;
							OutResult.ContributionProvider = Contributor.Name;
							return Finish(ECookRunStatus::Failed, ECookRunError::ContributionFailed);
						}
						if (Injected(ECookOperationStage::Capture, Index))
							return Finish(ECookRunStatus::Failed, ECookRunError::CaptureInjectedFailure);
						std::vector<FCookSavePlan> PackagePlans;
						const auto Captured = Context.TakeSavePlans(PackagePlans);
						if (!Captured || PackagePlans.size() != 1)
						{
							FCookCaptureResult Failure{.Error = Captured ? ECookCaptureError::PlanCount : ECookCaptureError::Finalization,
								.Package = Path, .Contributor = Contributor.Name, .ActualPlans = PackagePlans.size()};
							if (!Captured) Failure.PlanCause = Captured.Error;
							OutResult.CaptureCause = std::make_shared<FCookCaptureResult>(std::move(Failure));
							return Finish(ECookRunStatus::Failed, ECookRunError::CaptureFailed);
						}
						FCookSavePlan Plan = std::move(PackagePlans.front());
						Plan.InputFingerprint = Fingerprint;
						Plan.Contributor = Contributor.Name;
						Plan.ContributorVersion = Contributor.ContributorVersion;
						Plan.FamilyProducerVersion = Contributor.FamilyProducerVersion;
						const ECookPackageStatus PreparationStatus =
							Contributor.ClassifyPreparation ? Contributor.ClassifyPreparation(*Asset) : ECookPackageStatus::Captured;
						Plan.BuildProvenance = CookPackageStatusName(PreparationStatus);
						if (!RetainOutput(Plan.PackageFileSize, Plan.SegmentFileSize)) return Finish(ECookRunStatus::Failed, ECookRunError::OutputLimit);
						OutResult.ChangedBytes += Plan.PackageFileSize + Plan.SegmentFileSize;
						NewState.Entries.push_back(MakeCookStateEntry(Plan));
						OutResult.Packages.push_back({{}, Path, Plan.Contributor, PreparationStatus, ECookOperationStage::Capture, Plan.PackageFileSize, Plan.SegmentFileSize});
						NewState.Entries.back().BuildDependencies = Dependencies;
						if (auto Result = CheckInputs(); !Result) return InputFailure(Result);
						Plans.push_back(std::move(Plan));
					}
					if (Request.ReportProgress) Request.ReportProgress({ECookOperationStage::StageAuxiliary, {}, Packages.size(), Packages.size()});
					if (Injected(ECookOperationStage::StageAuxiliary, 0))
						return Finish(ECookRunStatus::Failed, ECookRunError::AuxiliaryInjectedFailure);
					FByteBuffer ShaderBytes;
					if (const auto ShaderResult = BuildCookedShaderLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, ShaderBytes, Request.IsCancelled); !ShaderResult)
					{
						OutResult.ShaderCause = std::make_shared<FShaderError>(ShaderResult.error());
						return IsCancelled(Request.IsCancelled)
							? Finish(ECookRunStatus::Cancelled, ECookRunError::Cancelled)
							: Finish(ECookRunStatus::Failed, ECookRunError::ShaderLibraryFailed);
					}
					if (!RetainOutput(ShaderBytes.size(), 0)) return Finish(ECookRunStatus::Failed, ECookRunError::OutputLimit);
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
				return InputFailure({ECookInputStatus::InvalidDependency, Error.what()});
			}
			if (!bPrepared) return false;
		}
		Contributors.clear();
		std::ranges::sort(Plans, {}, &FCookSavePlan::VirtualPath);
		std::ranges::sort(NewState.Entries, {}, &FCookStateEntry::VirtualPackagePath);
		if (Request.bDryRun) return Finish(ECookRunStatus::Succeeded, ECookRunError::None);
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
			OutResult.PublicationCause = std::make_shared<FCookPublishResult>(PublishResult);
			if (PublishResult.Status == ECookPublishStatus::Cancelled)
				return Finish(ECookRunStatus::Cancelled, ECookRunError::Cancelled);
			return Finish(ECookRunStatus::Failed, ECookRunError::PublicationFailed);
		}
		return Finish(ECookRunStatus::Succeeded, ECookRunError::None);
	}
} // namespace Durin
