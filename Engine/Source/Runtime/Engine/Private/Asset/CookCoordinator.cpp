#include "CookBuildProviders.h"
#include "CookInputCapture.h"
#include "AssetLiveLoadGuard.h"
#include "Asset/AssetCompilingManager.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "Modules/ModuleManager.h"
#include "Asset/Cook.h"
#include "Shader/ShaderBuildProvider.h"

#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "Asset/References.h"
#include "Asset/MutationExtensions.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/References.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "Engine/ProjectGameSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Project.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		const std::thread::id CookBootstrapOwner = std::this_thread::get_id();
		constexpr uint32 CookStateMagic = 0x54414e53; // SNAT
		constexpr uint32 CookStateVersion = 2;
		constexpr uint64 MaximumCookStateBytes = 256ull * 1024 * 1024;
		constexpr uint32 MaximumCookStateEntries = 1'000'000;
		constexpr uint8 CookStateSegmentRawFieldProjection = 1 << 0;
		constexpr uint8 CookStateSegmentOpaque = 1 << 1;

		auto Failure(EAssetError Error, std::string Message) -> FAssetResult
		{
			return {Error, std::move(Message)};
		}

		auto CookFail(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto AppendString(FBinaryWriter& Writer, std::string_view Value) -> bool
		{
			if (Value.empty() || Value.find('\0') != std::string_view::npos || Value.size() > 4096
				|| Value.size() > std::numeric_limits<uint32>::max()) return false;
			Writer.WriteU32(static_cast<uint32>(Value.size()));
			Writer.WriteBytes(std::as_bytes(std::span(Value)));
			return !Writer.HasError();
		}

		class FStateReader
		{
		public:
			explicit FStateReader(FByteView InBytes)
				: Reader(InBytes, {MaximumCookStateBytes, MaximumCookDependencyBytes})
			{
			}

			template<typename TValue>
			auto Read(TValue& OutValue) -> bool
			{
				return Reader.ReadInteger(OutValue);
			}

			auto ReadString(std::string& OutValue) -> bool
			{
				uint32 Size = 0;
				if (!Read(Size) || Size == 0 || Size > 4096
					|| Size > Reader.GetRemainingBytes()) return false;
				FByteView Encoded;
				if (!Reader.ReadRegion(Encoded, Size, 4096)) return false;
				OutValue.assign(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
				return OutValue.find('\0') == std::string::npos;
			}

			auto ReadDependencies(std::vector<FCookBuildDependency>& Out) -> bool
			{
				uint32 Size = 0;
				FByteView Bytes;
				return Read(Size) && Size <= MaximumCookDependencyBytes
					&& Reader.ReadRegion(Bytes, Size, MaximumCookDependencyBytes)
					&& DecodeCookBuildDependencies(Bytes, Out);
			}

			auto GetRemainingBytes() const -> uint64 { return Reader.GetRemainingBytes(); }

			auto IsAtEnd() const -> bool { return Reader.IsAtEnd(); }

		private:
			FBinaryReader Reader;
		};

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

		auto RelativePackagePath(std::string_view VirtualPath) -> std::string
		{
			return std::format("{}.dasset", VirtualPath.substr(1));
		}

		auto RelativeSegmentPath(std::string_view VirtualPath) -> std::string
		{
			return std::format("{}.dbulk", VirtualPath.substr(1));
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

		auto ValidateExistingFile(const std::filesystem::path& Path, uint64 ExpectedSize, const FXxHash128& ExpectedDigest) -> bool
		{
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(Path, ErrorCode) || ErrorCode
				|| std::filesystem::file_size(Path, ErrorCode) != ExpectedSize || ErrorCode)
				return false;
			FXxHash128 Digest;
			return FFileHelper::HashFileXx128(Path, Digest, ErrorCode)
				   && !ErrorCode && Digest == ExpectedDigest;
		}

		auto MakeTopLevelObjectPath(
			const FTopLevelAssetPath& AssetPath, FObjectPath& OutPath) -> bool
		{
			return FObjectPath::TryCreate(
				AssetPath, std::span<const std::string>{}, OutPath);
		}

		struct FOutputRecord
		{
			ECookManifestEntryKind Kind = ECookManifestEntryKind::CookedPackage;
			uint8 Flags = CookManifestEntryPresent;
			std::string RelativePath;
			FByteView Bytes;
			uint64 Size = 0;
			FXxHash128 Digest;
			ECookOperationStage Stage = ECookOperationStage::StagePackage;
			ECookOperationStage CommitStage = ECookOperationStage::CommitPackage;
			bool bReuse = false;
		};

		class FLocalLooseCookOutputStore final : public ICookOutputStore
		{
		public:
			FLocalLooseCookOutputStore(std::filesystem::path InRoot, ECookTargetPlatform InPlatform, ECookTargetProfile InProfile)
				: Root(std::move(InRoot).lexically_normal())
				, Platform(InPlatform)
				, Profile(InProfile)
			{
			}

			auto Publish(std::span<const FCookSavePlan> Plans,
				std::span<const FCookAuxiliaryOutput> AuxiliaryOutputs,
				const FCookState& State,
				FCookRunResult& InOutResult,
				const FCookCancellationCheck& Cancellation,
				const FCookFailureInjection& ShouldFail) -> FCookPublishResult override
			{
				std::string Error;
				bool bCancelled = false;
				if (PublishInternal(Plans, AuxiliaryOutputs, State, InOutResult,
					Cancellation, ShouldFail, Error, bCancelled))
					return {ECookPublishStatus::Succeeded, {}};
				return {bCancelled ? ECookPublishStatus::Cancelled
					: ECookPublishStatus::Failed, std::move(Error)};
			}

		private:
			auto PublishInternal(std::span<const FCookSavePlan> Plans,
				std::span<const FCookAuxiliaryOutput> AuxiliaryOutputs,
				const FCookState& State,
				FCookRunResult& InOutResult,
				const FCookCancellationCheck& Cancellation,
				const FCookFailureInjection& ShouldFail,
				std::string& OutError,
				bool& bOutCancelled) -> bool
			{
				bOutCancelled = false;
				const auto CommitStart = std::chrono::steady_clock::now();
				if (Root.empty() || !Root.is_absolute()
					|| State.TargetPlatform != Platform || State.TargetProfile != Profile)
					return CookFail("CookOutputStoreInvalidRequest: output root or target is invalid.", &OutError);
				for (size_t Index = 0; Index < Plans.size(); ++Index)
				{
					const FCookSavePlan& Plan = Plans[Index];
					std::filesystem::path PackagePath;
					if (Plan.TargetPlatform != Platform || Plan.TargetProfile != Profile
						|| Plan.PackageFileSize == 0
						|| Plan.PackageBytes.size() != Plan.PackageFileSize
						|| Plan.BulkBytes.size() != Plan.SegmentFileSize
						|| FXxHash128::HashBuffer(Plan.PackageBytes) != Plan.PackageDigest
						|| FXxHash128::HashBuffer(Plan.BulkBytes) != Plan.SegmentDigest
						|| !ResolveCookedPackagePath(
							Root, Plan.VirtualPath, PackagePath, &OutError
						)
						|| (Index && !(Plans[Index - 1].VirtualPath < Plan.VirtualPath)))
						return CookFail(OutError.empty() ? "CookOutputStoreInvalidPlan: save plans are invalid, duplicated, or unsorted." : OutError, &OutError);
					if (Plan.bOpaqueRawSegment)
					{
						if (Plan.BulkSummary.Extent != Plan.SegmentFileSize
							|| Plan.BulkSummary.Digest != Plan.SegmentDigest)
							return CookFail("CookOutputStoreInvalidOpaqueSegment", &OutError);
						continue;
					}
					FPackagePath VirtualPath;
					if (!FPackagePath::TryCreate(Plan.VirtualPath, VirtualPath)
						&& !FPackagePath::TryCreateProjectContent(
							Plan.VirtualPath, VirtualPath))
						return CookFail("CookOutputStoreInvalidPackageIdentity", &OutError);
					const FAssetResult PackageValidation = ValidateAssetPackageBytes(
						Plan.PackageBytes, VirtualPath, Plan.BulkBytes);
					if (!PackageValidation)
						return CookFail(std::format("CookOutputStoreInvalidPackage: {}: {}", Plan.VirtualPath, PackageValidation.Message), &OutError);
					if (Plan.SegmentFileSize == 0) continue;
					if (Plan.bRawBulkSegment
						&& (Plan.BulkSummary.Extent != Plan.SegmentFileSize
							|| Plan.BulkSummary.Digest != Plan.SegmentDigest))
						return CookFail("CookOutputStoreInvalidRawBulkClosure", &OutError);
				}
				for (size_t Index = 0; Index < AuxiliaryOutputs.size(); ++Index)
				{
					const FCookAuxiliaryOutput& Output = AuxiliaryOutputs[Index];
					const std::filesystem::path Relative(Output.RelativePath);
					if (Output.Kind != ECookManifestEntryKind::ShaderLibrary
						|| Output.RelativePath.empty() || Relative.is_absolute()
						|| Relative.lexically_normal() != Relative
						|| Relative.native().starts_with(std::filesystem::path("..").native())
						|| Output.Bytes.empty()
						|| FXxHash128::HashBuffer(Output.Bytes) != Output.Digest
						|| (Index && !(AuxiliaryOutputs[Index - 1].RelativePath < Output.RelativePath)))
						return CookFail("CookOutputStoreInvalidAuxiliaryOutput", &OutError);
				}
				std::error_code ErrorCode;
				std::filesystem::create_directories(Root, ErrorCode);
				if (ErrorCode) return CookFail(std::format("CookOutputStoreCreateRootFailed: {}", ErrorCode.message()), &OutError);

				const std::filesystem::path LockPath = Root / ".durin-cook-writer";
				if ((ShouldFail && ShouldFail(ECookOperationStage::WriterLock, 0, OutError))
					|| !std::filesystem::create_directory(LockPath, ErrorCode))
					return CookFail(OutError.empty() ? "CookCompetingWriter: the output root already has a writer." : OutError, &OutError);
				struct FLockCleanup
				{
					std::filesystem::path Path;
					~FLockCleanup()
					{
						std::error_code Error;
						std::filesystem::remove(Path, Error);
					}
				} LockCleanup{LockPath};

				static std::atomic_uint64_t NextTransaction{1};
				const std::filesystem::path TransactionRoot = Root / std::format(".durin-cook-transaction-{}", NextTransaction.fetch_add(1));
				const std::filesystem::path StagedRoot = TransactionRoot / "staged";
				const std::filesystem::path BackupRoot = TransactionRoot / "backup";
				std::filesystem::create_directories(StagedRoot, ErrorCode);
				if (ErrorCode) return CookFail("CookTransactionCreateFailed: could not create staging root.", &OutError);
				struct FTransactionCleanup
				{
					std::filesystem::path Path;
					~FTransactionCleanup()
					{
						std::error_code Error;
						std::filesystem::remove_all(Path, Error);
					}
				} TransactionCleanup{TransactionRoot};

				FCookManifest PreviousManifest;
				FByteBuffer PreviousManifestBytes;
				const bool bHasPreviousManifest = FFileHelper::LoadFileToArray(
													  PreviousManifestBytes, Root / "CookManifest.bin"
												  )
												  && DecodeCookManifest(PreviousManifestBytes, PreviousManifest);

				std::vector<FOutputRecord> Outputs;
				FCookManifest Manifest{Platform, Profile};
				for (const FCookSavePlan& Plan : Plans)
				{
					const std::string PackageRelative = RelativePackagePath(Plan.VirtualPath);
					Outputs.push_back({ECookManifestEntryKind::CookedPackage, static_cast<uint8>(CookManifestEntryPresent | (Plan.bRawBulkSegment ? CookManifestEntryCookedFieldProjection : 0)), PackageRelative, Plan.PackageBytes, Plan.PackageFileSize, Plan.PackageDigest, ECookOperationStage::StagePackage, ECookOperationStage::CommitPackage, Plan.bReuseExistingOutput});
					Manifest.Entries.push_back({ECookManifestEntryKind::CookedPackage, static_cast<uint8>(CookManifestEntryPresent | (Plan.bRawBulkSegment ? CookManifestEntryCookedFieldProjection : 0)), PackageRelative, Plan.PackageFileSize, Plan.PackageDigest.HashLow, Plan.PackageDigest.HashHigh});
					if (Plan.SegmentFileSize == 0) continue;
					const std::string SegmentRelative = RelativeSegmentPath(Plan.VirtualPath);
					Outputs.push_back({ECookManifestEntryKind::PackageBulk, CookManifestEntryPresent, SegmentRelative, Plan.BulkBytes, Plan.SegmentFileSize, Plan.SegmentDigest, ECookOperationStage::StageSegment, ECookOperationStage::CommitSegment, Plan.bReuseExistingOutput});
					Manifest.Entries.push_back({ECookManifestEntryKind::PackageBulk, CookManifestEntryPresent, SegmentRelative, Plan.SegmentFileSize, Plan.SegmentDigest.HashLow, Plan.SegmentDigest.HashHigh});
				}
				for (const FCookAuxiliaryOutput& Auxiliary : AuxiliaryOutputs)
				{
					Outputs.push_back({Auxiliary.Kind, CookManifestEntryPresent, Auxiliary.RelativePath, Auxiliary.Bytes, static_cast<uint64>(Auxiliary.Bytes.size()), Auxiliary.Digest, ECookOperationStage::StageAuxiliary, ECookOperationStage::CommitAuxiliary, false});
					Manifest.Entries.push_back({Auxiliary.Kind, CookManifestEntryPresent, Auxiliary.RelativePath, static_cast<uint64>(Auxiliary.Bytes.size()), Auxiliary.Digest.HashLow, Auxiliary.Digest.HashHigh});
				}

				std::ranges::stable_sort(Outputs, [](const FOutputRecord& Left, const FOutputRecord& Right) {
					if (Left.CommitStage != Right.CommitStage)
						return Left.CommitStage < Right.CommitStage;
					return Left.RelativePath < Right.RelativePath;
				});
				FByteBuffer ManifestBytes;
				FByteBuffer StateBytes;
				if (!EncodeCookManifest(Manifest, ManifestBytes, &OutError)
					|| !EncodeCookState(State, StateBytes, &OutError)) return false;

				auto StageBytes = [&](std::string_view Relative,
									  FByteView Bytes, ECookOperationStage Stage,
									  size_t Index) -> bool {
					if (IsCancelled(Cancellation))
					{
						bOutCancelled = true;
						return CookFail("CookCancelledDuringStaging", &OutError);
					}
					if (ShouldFail && ShouldFail(Stage, Index, OutError)) return false;
					const std::filesystem::path Staged = StagedRoot / Relative;
					std::filesystem::create_directories(Staged.parent_path(), ErrorCode);
					if (ErrorCode || !FFileHelper::SaveArrayToFile(Bytes, Staged))
						return CookFail(std::format("CookStageWriteFailed: {}", Relative), &OutError);
					FByteBuffer Validation;
					if (!FFileHelper::LoadFileToArray(Validation, Staged)
						|| !std::ranges::equal(Validation, Bytes))
						return CookFail(std::format("CookStageValidationFailed: {}", Relative), &OutError);
					return true;
				};

				for (size_t Index = 0; Index < Outputs.size(); ++Index)
				{
					FOutputRecord& Output = Outputs[Index];
					Output.bReuse = Output.bReuse && ValidateExistingFile(Root / Output.RelativePath, Output.Size, Output.Digest);
					if (!Output.bReuse && !StageBytes(Output.RelativePath, Output.Bytes, Output.Stage, Index)) return false;
				}
				const FXxHash128 StateDigest = FXxHash128::HashBuffer(StateBytes);
				const FXxHash128 ManifestDigest = FXxHash128::HashBuffer(ManifestBytes);
				const bool bReuseState = ValidateExistingFile(
					Root / "CookState.bin", StateBytes.size(), StateDigest
				);
				const bool bReuseManifest = ValidateExistingFile(
					Root / "CookManifest.bin", ManifestBytes.size(), ManifestDigest
				);
				if (!bReuseState && !StageBytes("CookState.bin", StateBytes, ECookOperationStage::CommitState, Outputs.size())) return false;
				if (!bReuseManifest && !StageBytes("CookManifest.bin", ManifestBytes, ECookOperationStage::CommitManifest, Outputs.size() + 1)) return false;

				struct FCommitted
				{
					std::filesystem::path Destination;
					std::filesystem::path Backup;
					bool bHadBackup = false;
				};
				std::vector<FCommitted> Committed;
				auto Rollback = [&]() {
					const auto Start = std::chrono::steady_clock::now();
					if (ShouldFail)
					{
						std::string Ignored;
						(void)ShouldFail(ECookOperationStage::Rollback, 0, Ignored);
					}
					for (auto It = Committed.rbegin(); It != Committed.rend(); ++It)
					{
						std::filesystem::remove(It->Destination, ErrorCode);
						if (It->bHadBackup)
							std::filesystem::rename(It->Backup, It->Destination, ErrorCode);
					}
					InOutResult.RollbackTimeNanoseconds += std::chrono::duration_cast<
															   std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start)
															   .count();
				};
				auto CommitFile = [&](std::string_view Relative,
									  ECookOperationStage Stage, size_t Index) -> bool {
					if (IsCancelled(Cancellation))
					{
						bOutCancelled = true;
						return CookFail("CookCancelledDuringCommit", &OutError);
					}
					if (ShouldFail && ShouldFail(Stage, Index, OutError)) return false;
					const std::filesystem::path Destination = Root / Relative;
					const std::filesystem::path Staged = StagedRoot / Relative;
					const std::filesystem::path Backup = BackupRoot / Relative;
					std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
					if (ErrorCode) return CookFail("CookCommitCreateDirectoryFailed", &OutError);
					const bool bExists = std::filesystem::exists(Destination, ErrorCode) && !ErrorCode;
					if (bExists)
					{
						std::filesystem::create_directories(Backup.parent_path(), ErrorCode);
						std::filesystem::rename(Destination, Backup, ErrorCode);
						if (ErrorCode) return CookFail(std::format("CookCommitBackupFailed: {}", Relative), &OutError);
					}
					Committed.push_back({Destination, Backup, bExists});
					std::filesystem::rename(Staged, Destination, ErrorCode);
					if (ErrorCode) return CookFail(std::format("CookCommitReplaceFailed: {}", Relative), &OutError);
					return true;
				};

				for (size_t Index = 0; Index < Outputs.size(); ++Index)
					if (!Outputs[Index].bReuse && !CommitFile(Outputs[Index].RelativePath, Outputs[Index].CommitStage, Index))
					{
						Rollback();
						return false;
					}
				if (!bReuseState && !CommitFile("CookState.bin", ECookOperationStage::CommitState, Outputs.size()))
				{
					Rollback();
					return false;
				}
				if (!bReuseManifest && !CommitFile("CookManifest.bin", ECookOperationStage::CommitManifest, Outputs.size() + 1))
				{
					Rollback();
					return false;
				}

				if (bHasPreviousManifest)
				{
					std::unordered_set<std::string> Current;
					for (const FCookManifestEntry& Entry : Manifest.Entries)
						Current.insert(Entry.RelativePath);
					for (size_t Index = 0; Index < PreviousManifest.Entries.size(); ++Index)
					{
						const FCookManifestEntry& Entry = PreviousManifest.Entries[Index];
						if (Current.contains(Entry.RelativePath)) continue;
						if (ShouldFail && ShouldFail(ECookOperationStage::StaleCleanup, Index, OutError)) break;
						const std::filesystem::path Candidate = (Root / Entry.RelativePath).lexically_normal();
						const std::filesystem::path Relative = Candidate.lexically_relative(Root);
						if (Relative.empty() || Relative.native().starts_with(std::filesystem::path("..").native())) continue;
						std::filesystem::remove(Candidate, ErrorCode);
					}
				}
				InOutResult.CommitTimeNanoseconds += std::chrono::duration_cast<
														 std::chrono::nanoseconds>(std::chrono::steady_clock::now() - CommitStart)
														 .count();
				OutError.clear();
				return true;
			}

			std::filesystem::path Root;
			ECookTargetPlatform Platform = ECookTargetPlatform::Invalid;
			ECookTargetProfile Profile = ECookTargetProfile::Invalid;
		};
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

	auto EncodeCookState(const FCookState& State, FByteBuffer& OutBytes, std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (State.TargetPlatform != ECookTargetPlatform::Win64
			|| (State.TargetProfile != ECookTargetProfile::Game
				&& State.TargetProfile != ECookTargetProfile::EditorValidation)
			|| State.Entries.size() > MaximumCookStateEntries)
			return CookFail("Cook state header is invalid.", OutError);
		std::vector<const FCookStateEntry*> Entries;
		Entries.reserve(State.Entries.size());
		for (const FCookStateEntry& Entry : State.Entries)
			Entries.push_back(&Entry);
		std::ranges::sort(Entries, {}, &FCookStateEntry::VirtualPackagePath);
		for (size_t Index = 1; Index < Entries.size(); ++Index)
			if (Entries[Index - 1]->VirtualPackagePath == Entries[Index]->VirtualPackagePath)
				return CookFail("Cook state contains a duplicate package path.", OutError);
		FBinaryWriter Writer({MaximumCookStateBytes, MaximumCookDependencyBytes});
		Writer.WriteU32(CookStateMagic);
		Writer.WriteU32(CookStateVersion);
		Writer.WriteU32(static_cast<uint32>(State.TargetPlatform));
		Writer.WriteU32(static_cast<uint32>(State.TargetProfile));
		Writer.WriteU32(static_cast<uint32>(Entries.size()));
		Writer.WriteU32(0);
		for (const FCookStateEntry* Entry : Entries)
		{
			if (!AppendString(Writer, Entry->VirtualPackagePath)
				|| !AppendString(Writer, Entry->Contributor)
				|| !AppendString(Writer, Entry->BuildProvenance))
				return CookFail("Cook state string is invalid or exceeds its bound.", OutError);
			Writer.WriteHash128(Entry->InputFingerprint);
			Writer.WriteHash128(Entry->PackageDigest);
			Writer.WriteHash128(Entry->SegmentDigest);
			Writer.WriteU64(Entry->PackageSize);
			Writer.WriteU64(Entry->SegmentSize);
			Writer.WriteU32(Entry->ContributorVersion);
			Writer.WriteU32(Entry->FamilyProducerVersion);
			Writer.WriteU8(Entry->SegmentFlags);
			FByteBuffer Dependencies;
			if (!EncodeCookBuildDependencies(Entry->BuildDependencies, Dependencies, OutError)) return false;
			Writer.WriteU32(static_cast<uint32>(Dependencies.size()));
			Writer.WriteBytes(Dependencies);
		}
		if (Writer.HasError())
			return CookFail("Cook state exceeds its byte bound.", OutError);
		OutBytes = Writer.TakeBytes();
		if (OutError) OutError->clear();
		return true;
	}

	auto DecodeCookState(FByteView Bytes, FCookState& OutState, std::string* OutError) -> bool
	{
		OutState = {};
		if (Bytes.size() > MaximumCookStateBytes)
			return CookFail("Cook state exceeds its byte bound.", OutError);
		FStateReader Reader(Bytes);
		uint32 Magic = 0, Version = 0, Platform = 0, Profile = 0, Count = 0, Reserved = 0;
		if (!Reader.Read(Magic) || !Reader.Read(Version) || !Reader.Read(Platform)
			|| !Reader.Read(Profile) || !Reader.Read(Count) || !Reader.Read(Reserved)
			|| Magic != CookStateMagic || Version != CookStateVersion || Reserved != 0
			|| Count > MaximumCookStateEntries || Count > Reader.GetRemainingBytes() / 100
			|| Platform != static_cast<uint32>(ECookTargetPlatform::Win64)
			|| (Profile != static_cast<uint32>(ECookTargetProfile::Game)
				&& Profile != static_cast<uint32>(ECookTargetProfile::EditorValidation)))
			return CookFail("Cook state header is unsupported or corrupt.", OutError);
		FCookState Candidate{
			static_cast<ECookTargetPlatform>(Platform),
			static_cast<ECookTargetProfile>(Profile)
		};
		Candidate.Entries.reserve(Count);
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			FCookStateEntry Entry;
			if (!Reader.ReadString(Entry.VirtualPackagePath)
				|| !Reader.ReadString(Entry.Contributor)
				|| !Reader.ReadString(Entry.BuildProvenance)
				|| !Reader.Read(Entry.InputFingerprint.HashLow)
				|| !Reader.Read(Entry.InputFingerprint.HashHigh)
				|| !Reader.Read(Entry.PackageDigest.HashLow)
				|| !Reader.Read(Entry.PackageDigest.HashHigh)
				|| !Reader.Read(Entry.SegmentDigest.HashLow)
				|| !Reader.Read(Entry.SegmentDigest.HashHigh)
				|| !Reader.Read(Entry.PackageSize) || !Reader.Read(Entry.SegmentSize)
				|| !Reader.Read(Entry.ContributorVersion)
				|| !Reader.Read(Entry.FamilyProducerVersion)
				|| !Reader.Read(Entry.SegmentFlags)
				|| !Reader.ReadDependencies(Entry.BuildDependencies)
				|| (Index && !(Candidate.Entries.back().VirtualPackagePath < Entry.VirtualPackagePath)) || Entry.PackageSize == 0
				|| (Entry.SegmentFlags & ~(CookStateSegmentRawFieldProjection | CookStateSegmentOpaque)) != 0)
				return CookFail("Cook state entry is corrupt or noncanonical.", OutError);
			Candidate.Entries.push_back(std::move(Entry));
		}
		if (!Reader.IsAtEnd()) return CookFail("Cook state has trailing bytes.", OutError);
		OutState = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}

	auto CreateLocalLooseCookOutputStore(std::filesystem::path OutputRoot, ECookTargetPlatform TargetPlatform, ECookTargetProfile TargetProfile)
		-> std::unique_ptr<ICookOutputStore>
	{
		return std::make_unique<FLocalLooseCookOutputStore>(
			std::move(OutputRoot), TargetPlatform, TargetProfile
		);
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
		if (GIsGameThreadIdInitialized ? !IsInGameThread() : std::this_thread::get_id() != CookBootstrapOwner)
			return Finish(ECookRunStatus::Failed, "wrong-thread", "Cook requires the object owner thread.");
		static std::atomic_flag Running = ATOMIC_FLAG_INIT;
		if (Running.test_and_set()) return Finish(ECookRunStatus::Failed, "capture-in-use", "A Cook capture is already active.");
		struct FRunGuard { std::atomic_flag& Flag; ~FRunGuard() { Flag.clear(); } } RunGuard{Running};
		std::vector<std::shared_ptr<void>> CodeLeases;
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
		uint64 CapturedBytes = 0;
		FAssetCompilingManager::Get().FinishAllCompilation();
		std::unordered_set<FName> Modules;
		for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
			if (const auto* Package = Object->GetPackage())
			{
				const std::string Name = Package->GetPackagePath();
				if (!Name.starts_with("/Cpp/")) continue;
				const FName Module(Name.substr(5));
				if (!Modules.insert(Module).second || !FModuleManager::Get().IsModuleLoaded(Module)) continue;
				auto Lease = FModuleManager::Get().AcquireCodeLease(Module);
				if (!Lease) return Finish(ECookRunStatus::Failed, "provider-unavailable", "Could not retain reflected module code.");
				CodeLeases.push_back(std::move(Lease));
			}
		{
			FScopedTypeRegistrationFreeze Types;
			AssetPrivate::FAssetLiveLoadGuard LiveOperations(true);
			AssetPrivate::FCookInputCapture Inputs(Request, CaptureAssetRegistrySnapshot(),
				[&](const FAssetData& Data, FCookContributorRegistration& Out) -> FAssetResult {
					if (Data.TopLevelAssets.empty()) return Failure(EAssetError::InvalidPackageType, "Cook package has no assets.");
					std::shared_ptr<const FRegisteredCookContributor> Entry;
					const auto Result = ResolveCookContributor(FindClassByQualifiedName(FName(
						Data.TopLevelAssets.front().AssetClassName)), Contributors, Entry);
					if (Result) Out = Entry->Registration;
					return Result;
				});
			auto CheckInputs = [&]() -> FAssetResult {
				if (Types.WasRegistrationRejected()) return {EAssetError::InUse, "Reflected registration changed during Cook capture."};
				if (!AssetPrivate::VerifyCapturedCookBuildProviders()) return {EAssetError::InUse, "Cook recipe provider changed during capture."};
				if (auto Result = LiveOperations.GetFailure(); !Result) return Result;
				if (auto Result = Inputs.Verify(); !Result) return Result;
				if (Types.WasRegistrationRejected()) return {EAssetError::InUse, "Reflected registration changed during Cook callback."};
				return LiveOperations.GetFailure();
			};
			auto RetainOutput = [&](uint64 PackageBytes, uint64 BulkBytes) -> bool {
				constexpr uint64 MaximumOutputBytes = 1024ull * 1024 * 1024;
				if (PackageBytes > MaximumOutputBytes - CapturedBytes
					|| BulkBytes > MaximumOutputBytes - CapturedBytes - PackageBytes)
				{
					OutResult.InputStatus = ECookInputStatus::LimitExceeded;
					OutResult.InputFailure = {EAssetError::CorruptFile, "Cook detached output byte limit exceeded."};
					return false;
				}
				CapturedBytes += PackageBytes + BulkBytes;
				OutResult.PeakCapturedBytes = std::max(OutResult.PeakCapturedBytes, CapturedBytes + Inputs.GetRetainedBytes());
				return true;
			};
			auto CaptureFailure = [&](const FAssetResult& Result) -> bool {
				OutResult.InputFailure = Result;
				OutResult.InputStatus = Inputs.GetStatus() == ECookInputStatus::None
					? ECookInputStatus::InputChanged : Inputs.GetStatus();
				return Finish(Inputs.GetPhase() == AssetPrivate::ECookCapturePhase::Cancelled
					? ECookRunStatus::Cancelled : ECookRunStatus::Failed, "input-capture-failed", Result.Message);
			};
			std::string ShaderError;
			bool bCaptured = false;
			bool bShaderCancelled = false;
			try
			{
				bCaptured = AssetPrivate::WithCapturedCookBuildProviders([&]() -> bool {
				return WithCapturedShaderBuildInputs([&]() -> bool {
					FAssetReferenceStoreCapture ExternalRoots;
					if (auto Result = CaptureAssetReferenceStores(ExternalRoots); !Result) return CaptureFailure(Result);
					if (auto Result = Inputs.Acquire(Roots, ExternalRoots); !Result) return CaptureFailure(Result);
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
						if (auto Result = CheckInputs(); !Result) return CaptureFailure(Result);
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
						FByteBuffer ExistingPackageBytes;
						FByteBuffer ExistingSegmentBytes;
						if (bCookHit)
						{
							bCookHit = ReadBoundedCookFile(Request.OutputRoot / RelativePackagePath(Path.GetView()), ExistingPackageBytes, [&] { return CheckInputs().Succeeded(); });
							if (bCookHit && Prior->second->SegmentSize != 0)
								bCookHit = ReadBoundedCookFile(Request.OutputRoot / RelativeSegmentPath(Path.GetView()), ExistingSegmentBytes, [&] { return CheckInputs().Succeeded(); });
						}
						if (auto Result = CheckInputs(); !Result) return CaptureFailure(Result);
						if (bCookHit)
							bCookHit = ExistingPackageBytes.size() == Prior->second->PackageSize
								&& ExistingSegmentBytes.size() == Prior->second->SegmentSize
								&& FXxHash128::HashBuffer(ExistingPackageBytes) == Prior->second->PackageDigest
								&& FXxHash128::HashBuffer(ExistingSegmentBytes) == Prior->second->SegmentDigest;
						if (bCookHit)
						{
							const FCookStateEntry& Hit = *Prior->second;
							if (!RetainOutput(Hit.PackageSize, Hit.SegmentSize)) return Finish(ECookRunStatus::Failed, "output-limit", OutResult.InputFailure.Message);
							Plans.push_back({.VirtualPath = Hit.VirtualPackagePath, .PackageBytes = std::move(ExistingPackageBytes), .BulkBytes = std::move(ExistingSegmentBytes), .InputFingerprint = Hit.InputFingerprint, .PackageDigest = Hit.PackageDigest, .SegmentDigest = Hit.SegmentDigest, .PackageFileSize = Hit.PackageSize, .SegmentFileSize = Hit.SegmentSize, .TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile, .ContributorVersion = Hit.ContributorVersion, .FamilyProducerVersion = Hit.FamilyProducerVersion, .Contributor = Hit.Contributor, .BuildProvenance = Hit.BuildProvenance, .bRawBulkSegment = (Hit.SegmentFlags & CookStateSegmentRawFieldProjection) != 0, .bOpaqueRawSegment = (Hit.SegmentFlags & CookStateSegmentOpaque) != 0, .bReuseExistingOutput = true});
							Plans.back().BulkSummary = {Hit.SegmentSize, Hit.SegmentDigest};
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
						Inputs.SetCurrentPackage(Path);
						const FAssetResult LoadResult = Inputs.LoadObject(CookRootPath, Asset);
						if (!LoadResult || !Asset) return CaptureFailure(LoadResult);
						FCookContext Capture({}, Request.TargetPlatform, Request.TargetProfile, Request.bRetainEditorOnlyData);

						if (ShouldFail && ShouldFail(ECookOperationStage::Prepare, Index, Error))
							return Finish(ECookRunStatus::Failed, "prepare-injected-failure", Error);
						DPackage* AuthoredPackage = Asset->GetPackage();
						if (!AuthoredPackage)
							return Finish(ECookRunStatus::Failed, "missing-package", std::format("CookMissingPackage: package={}, contributor={}", Path.ToString(), Contributor.Name));
						const bool bWasDirty = AuthoredPackage->IsDirty();
						FByteBuffer AuthoredBytesBefore;
						const FAssetResult BeforeResult = SerializeAssetPackageBytes(
							AuthoredPackage, AuthoredBytesBefore
						);
						if (!BeforeResult)
							return Finish(ECookRunStatus::Failed, "authored-snapshot-failed", std::format("CookAuthoredSnapshotFailed: package={}, contributor={}: {}", Path.ToString(), Contributor.Name, BeforeResult.Message));
						const FAssetResult Contribution = Contributor.Contribute(
							*Asset, Path.GetView(), Capture
						);
						if (auto Result = CheckInputs(); !Result) return CaptureFailure(Result);
						if (!Contribution)
							return Finish(ECookRunStatus::Failed, "contribution-failed", std::format("CookContributionFailed: package={}, contributor={}, stage=prepare: {}", Path.ToString(), Contributor.Name, Contribution.Message));
						FByteBuffer AuthoredBytesAfter;
						const FAssetResult AfterResult = SerializeAssetPackageBytes(
							AuthoredPackage, AuthoredBytesAfter
						);
						if (!AfterResult || AuthoredPackage->IsDirty() != bWasDirty
							|| AuthoredBytesAfter != AuthoredBytesBefore)
							return Finish(ECookRunStatus::Failed, "contributor-mutated-authored-package", std::format("CookContributorMutatedAuthoredPackage: package={}, contributor={}", Path.ToString(), Contributor.Name));
						if (ShouldFail && ShouldFail(ECookOperationStage::Capture, Index, Error))
							return Finish(ECookRunStatus::Failed, "capture-injected-failure", Error);
						std::vector<FCookSavePlan> Captured;
						if (!Capture.TakeSavePlans(Captured, &Error) || Captured.size() != 1)
							return Finish(ECookRunStatus::Failed, "capture-failed", std::format("CookCaptureFailed: package={}, contributor={}: {}", Path.ToString(), Contributor.Name, Error));
						FCookSavePlan Plan = std::move(Captured.front());
						Plan.InputFingerprint = Fingerprint;
						Plan.Contributor = Contributor.Name;
						Plan.ContributorVersion = Contributor.ContributorVersion;
						Plan.FamilyProducerVersion = Contributor.FamilyProducerVersion;
						const ECookPackageStatus PreparationStatus =
							Contributor.ClassifyPreparation ? Contributor.ClassifyPreparation(*Asset) : ECookPackageStatus::Captured;
						Plan.BuildProvenance = CookPackageStatusName(PreparationStatus);
						if (!RetainOutput(Plan.PackageFileSize, Plan.SegmentFileSize)) return Finish(ECookRunStatus::Failed, "output-limit", OutResult.InputFailure.Message);
						OutResult.PeakCapturedBytes = std::max(OutResult.PeakCapturedBytes, CapturedBytes);
						OutResult.ChangedBytes += Plan.PackageFileSize + Plan.SegmentFileSize;
						NewState.Entries.push_back({Plan.VirtualPath, Plan.InputFingerprint, Plan.PackageDigest, Plan.SegmentDigest, Plan.PackageFileSize, Plan.SegmentFileSize, Plan.ContributorVersion, Plan.FamilyProducerVersion, Plan.Contributor, Plan.BuildProvenance});
						NewState.Entries.back().SegmentFlags = static_cast<uint8>(
							(Plan.bRawBulkSegment ? CookStateSegmentRawFieldProjection : 0)
							| (Plan.bOpaqueRawSegment ? CookStateSegmentOpaque : 0)
						);
						OutResult.Packages.push_back({{}, Path, Plan.Contributor, std::string(CookPackageStatusName(PreparationStatus)), "Captured deterministic package save plan.", PreparationStatus, ECookOperationStage::Capture, Plan.PackageFileSize, Plan.SegmentFileSize});
						NewState.Entries.back().BuildDependencies = Dependencies;
						if (auto Result = CheckInputs(); !Result) return CaptureFailure(Result);
						Plans.push_back(std::move(Plan));
					}
					if (Request.ReportProgress) Request.ReportProgress({ECookOperationStage::StageAuxiliary, {}, Packages.size(), Packages.size()});
					std::string Error;
					if (ShouldFail && ShouldFail(ECookOperationStage::StageAuxiliary, 0, Error))
						return Finish(ECookRunStatus::Failed, "auxiliary-injected-failure", Error);
					FByteBuffer ShaderBytes;
					if (!BuildCookedShaderLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, ShaderBytes, Error))
						return Finish(ECookRunStatus::Failed, "shader-library-failed", Error);
					if (!RetainOutput(ShaderBytes.size(), 0)) return Finish(ECookRunStatus::Failed, "output-limit", OutResult.InputFailure.Message);
					AuxiliaryOutputs.push_back({ECookManifestEntryKind::ShaderLibrary,
						std::string(ShaderCookedLibraryRelativePath), std::move(ShaderBytes)});
					AuxiliaryOutputs.back().Digest = FXxHash128::HashBuffer(AuxiliaryOutputs.back().Bytes);
					if (auto Result = CheckInputs(); !Result) return CaptureFailure(Result);
					OutResult.PeakCapturedBytes = std::max(OutResult.PeakCapturedBytes,
						CapturedBytes + Inputs.GetRetainedBytes());
					Inputs.Detach();
					return true;
				}, ShaderError, EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, [&] {
						bShaderCancelled |= IsCancelled(Request.IsCancelled);
						return bShaderCancelled;
					});
				}, ShaderError);
			}
			catch (const std::exception& Error)
			{
				return CaptureFailure({EAssetError::InUse, Error.what()});
			}
			if (!bCaptured)
			{
				if (!OutResult.Code.empty()) return false;
				if (bShaderCancelled) return Finish(ECookRunStatus::Cancelled, "cancelled", ShaderError);
				if (Types.WasRegistrationRejected() || !LiveOperations.GetFailure())
					return CaptureFailure({EAssetError::InUse, "A prohibited operation interrupted Cook capture."});
				OutResult.InputStatus = ECookInputStatus::ProviderUnavailable;
				return Finish(ECookRunStatus::Failed, "shader-input-capture-failed", ShaderError);
			}
		}
		Contributors.clear();
		CodeLeases.clear();
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
