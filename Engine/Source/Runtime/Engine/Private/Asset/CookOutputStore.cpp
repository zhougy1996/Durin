#include "CookOutputInternal.h"
#include "Misc/FileHelper.h"
#include "Asset/PackageInspection.h"
namespace Durin
{
	using namespace AssetPrivate;
	namespace
	{
		auto IsCancelled(const FCookCancellationCheck& Check) -> bool { return Check && Check(); }
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
				if (!ValidateCookOutputRoot(Root, OutError)) return false;
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
	}

	auto CreateLocalLooseCookOutputStore(std::filesystem::path OutputRoot, ECookTargetPlatform TargetPlatform, ECookTargetProfile TargetProfile)
		-> std::unique_ptr<ICookOutputStore>
	{
		return std::make_unique<FLocalLooseCookOutputStore>(
			std::move(OutputRoot), TargetPlatform, TargetProfile
		);
	}

}
