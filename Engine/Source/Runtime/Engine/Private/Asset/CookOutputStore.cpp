#include "CookOutputInternal.h"
#include "Misc/FileHelper.h"
#include "Asset/PackageInspection.h"
namespace Durin
{
	using namespace AssetPrivate;
	auto FormatCookPublishError(const FCookPublishResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case ECookPublishError::None: return {};
		case ECookPublishError::Root: if (Result.RootCause) return FormatCookOutputRootError(*Result.RootCause); break;
		case ECookPublishError::Path: if (Result.PathCause) return FormatCookedPathError(*Result.PathCause); break;
		case ECookPublishError::Package: if (!Result.PackageDiagnostic.empty()) return "CookOutputStoreInvalidPackage: " + Result.VirtualPath + ": " + Result.PackageDiagnostic; break;
		case ECookPublishError::Manifest: if (Result.ManifestCause) return FormatCookManifestError(*Result.ManifestCause); break;
		case ECookPublishError::State: if (Result.StateCause) return FormatCookStateError(*Result.StateCause); break;
		case ECookPublishError::Operation: if (Result.OperationCause) return FormatCookPublishOperationError(*Result.OperationCause); break;
		case ECookPublishError::Validation: if (Result.ValidationCause) return FormatCookPublishValidationError(*Result.ValidationCause); break;
		case ECookPublishError::Injected: if (Result.InjectionCause) return Result.InjectionCause->ExternalDiagnostic; break;
		case ECookPublishError::Unspecified: break;
		}
		return "Cook publication failed.";
	}

	auto FormatCookPublishValidationError(const FCookPublishValidationFailure& Failure) -> std::string
	{
		switch (Failure.Error)
		{
		case ECookPublishValidationError::Request: return "CookOutputStoreInvalidRequest: output root or target is invalid.";
		case ECookPublishValidationError::Plan: return "CookOutputStoreInvalidPlan: save plans are invalid, duplicated, or unsorted: " + Failure.Path;
		case ECookPublishValidationError::OpaqueSegment: return "CookOutputStoreInvalidOpaqueSegment: " + Failure.Path;
		case ECookPublishValidationError::PackageIdentity: return "CookOutputStoreInvalidPackageIdentity: " + Failure.Path;
		case ECookPublishValidationError::RawBulkClosure: return "CookOutputStoreInvalidRawBulkClosure: " + Failure.Path;
		case ECookPublishValidationError::AuxiliaryOutput: return "CookOutputStoreInvalidAuxiliaryOutput: " + Failure.Path;
		}
		return {};
	}

	auto FormatCookPublishOperationError(const FCookPublishOperationFailure& Failure) -> std::string
	{
		std::string_view Code;
		switch (Failure.Error)
		{
		case ECookPublishOperationError::CreateRoot: Code = "CookOutputStoreCreateRootFailed"; break;
		case ECookPublishOperationError::WriterLock: Code = "CookCompetingWriter"; break;
		case ECookPublishOperationError::CreateTransaction: Code = "CookTransactionCreateFailed"; break;
		case ECookPublishOperationError::CancelledStaging: Code = "CookCancelledDuringStaging"; break;
		case ECookPublishOperationError::CancelledCommit: Code = "CookCancelledDuringCommit"; break;
		case ECookPublishOperationError::StageWrite: Code = "CookStageWriteFailed"; break;
		case ECookPublishOperationError::StageValidation: Code = "CookStageValidationFailed"; break;
		case ECookPublishOperationError::CommitDirectory: Code = "CookCommitCreateDirectoryFailed"; break;
		case ECookPublishOperationError::CommitBackup: Code = "CookCommitBackupFailed"; break;
		case ECookPublishOperationError::CommitReplace: Code = "CookCommitReplaceFailed"; break;
		}
		return std::format("{}: {}{}", Code, Failure.Path.generic_string(),
			Failure.SystemError ? ": " + Failure.SystemError.message() : std::string{});
	}

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
				FCookPublishResult Failure;
				bool bCancelled = false;
				if (PublishInternal(Plans, AuxiliaryOutputs, State, InOutResult,
					Cancellation, ShouldFail, bCancelled, Failure))
					return {.Status = ECookPublishStatus::Succeeded, .Error = ECookPublishError::None};
				Failure.Status = bCancelled ? ECookPublishStatus::Cancelled : ECookPublishStatus::Failed;
				return Failure;
			}

		private:
			auto PublishInternal(std::span<const FCookSavePlan> Plans,
				std::span<const FCookAuxiliaryOutput> AuxiliaryOutputs,
				const FCookState& State,
				FCookRunResult& InOutResult,
				const FCookCancellationCheck& Cancellation,
				const FCookFailureInjection& ShouldFail,
				bool& bOutCancelled, FCookPublishResult& Failure) -> bool
			{
				bOutCancelled = false;
				auto RejectOperation = [&](ECookPublishOperationError Error, ECookOperationStage Stage,
					const std::filesystem::path& Path, std::error_code SystemError = {}) -> bool {
					Failure.OperationCause = FCookPublishOperationFailure{Error, Stage, Path, SystemError};
					Failure.Error = ECookPublishError::Operation;
					return false;
				};
				auto RejectValidation = [&](ECookPublishValidationError Error, std::string_view Path = {}, uint64 Index = 0) -> bool {
					Failure.ValidationCause = FCookPublishValidationFailure{Error, Root, std::string(Path), Index, State.TargetPlatform, State.TargetProfile};
					Failure.Error = ECookPublishError::Validation;
					return false;
				};
				auto Injected = [&](ECookOperationStage Stage, size_t Index) -> bool {
					std::string External;
					if (!ShouldFail || !ShouldFail(Stage, Index, External)) return false;
					External.resize(std::min<size_t>(External.size(), 2048));
					Failure.Error = ECookPublishError::Injected;
					Failure.InjectionCause = FCookPublishInjectedFailure{Stage, Index, std::move(External)};
					return true;
				};
				const auto CommitStart = std::chrono::steady_clock::now();
				if (Root.empty() || !Root.is_absolute()
					|| State.TargetPlatform != Platform || State.TargetProfile != Profile)
					return RejectValidation(ECookPublishValidationError::Request);
				if (const auto Validated = ValidateCookOutputRoot(Root); !Validated)
				{
					Failure.RootCause = std::make_shared<FCookOutputRootResult>(Validated);
					Failure.Error = ECookPublishError::Root;
					return false;
				}
				for (size_t Index = 0; Index < Plans.size(); ++Index)
				{
					const FCookSavePlan& Plan = Plans[Index];
					std::filesystem::path PackagePath;
					FCookedPathResult PathResult;
					if (Plan.TargetPlatform != Platform || Plan.TargetProfile != Profile
						|| Plan.PackageFileSize == 0
						|| Plan.PackageBytes.size() != Plan.PackageFileSize
						|| Plan.BulkBytes.size() != Plan.SegmentFileSize
						|| FXxHash128::HashBuffer(Plan.PackageBytes) != Plan.PackageDigest
						|| FXxHash128::HashBuffer(Plan.BulkBytes) != Plan.SegmentDigest
						|| !(PathResult = ResolveCookedPackagePath(
							Root, Plan.VirtualPath, PackagePath
						))
						|| (Index && !(Plans[Index - 1].VirtualPath < Plan.VirtualPath)))
					{
						Failure.VirtualPath = Plan.VirtualPath;
						if (!PathResult) Failure.PathCause = PathResult;
						if (!PathResult) { Failure.Error = ECookPublishError::Path; return false; }
						return RejectValidation(ECookPublishValidationError::Plan, Plan.VirtualPath, Index);
					}
					if (Plan.bOpaqueRawSegment)
					{
						if (Plan.BulkSummary.Extent != Plan.SegmentFileSize
							|| Plan.BulkSummary.Digest != Plan.SegmentDigest)
							return RejectValidation(ECookPublishValidationError::OpaqueSegment, Plan.VirtualPath, Index);
						continue;
					}
					FPackagePath VirtualPath;
					if (!FPackagePath::TryCreate(Plan.VirtualPath, VirtualPath)
						&& !FPackagePath::TryCreateProjectContent(
							Plan.VirtualPath, VirtualPath))
						return RejectValidation(ECookPublishValidationError::PackageIdentity, Plan.VirtualPath, Index);
					const auto PackageValidation = ValidateAssetPackageBytes(
						Plan.PackageBytes, VirtualPath, Plan.BulkBytes);
					if (!PackageValidation)
					{
						Failure.VirtualPath = Plan.VirtualPath;
						Failure.PackageDiagnostic = PackageValidation.Message;
						Failure.Error = ECookPublishError::Package;
						return false;
					}
					if (Plan.SegmentFileSize == 0) continue;
					if (Plan.bRawBulkSegment
						&& (Plan.BulkSummary.Extent != Plan.SegmentFileSize
							|| Plan.BulkSummary.Digest != Plan.SegmentDigest))
						return RejectValidation(ECookPublishValidationError::RawBulkClosure, Plan.VirtualPath, Index);
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
						return RejectValidation(ECookPublishValidationError::AuxiliaryOutput, Output.RelativePath, Index);
				}
				std::error_code ErrorCode;
				std::filesystem::create_directories(Root, ErrorCode);
				if (ErrorCode) return RejectOperation(ECookPublishOperationError::CreateRoot, ECookOperationStage::Prepare, Root, ErrorCode);

				const std::filesystem::path LockPath = Root / ".durin-cook-writer";
				if (Injected(ECookOperationStage::WriterLock, 0)) return false;
				if (!std::filesystem::create_directory(LockPath, ErrorCode))
					return RejectOperation(ECookPublishOperationError::WriterLock, ECookOperationStage::WriterLock, LockPath, ErrorCode);
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
				if (ErrorCode) return RejectOperation(ECookPublishOperationError::CreateTransaction, ECookOperationStage::Prepare, StagedRoot, ErrorCode);
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
				if (const auto Encoded = EncodeCookManifest(Manifest, ManifestBytes); !Encoded)
				{
					Failure.ManifestCause = Encoded;
					Failure.Error = ECookPublishError::Manifest;
					return false;
				}
				if (const auto Encoded = EncodeCookState(State, StateBytes); !Encoded)
				{
					Failure.StateCause = Encoded;
					Failure.Error = ECookPublishError::State;
					return false;
				}

				auto StageBytes = [&](std::string_view Relative,
									  FByteView Bytes, ECookOperationStage Stage,
									  size_t Index) -> bool {
					if (IsCancelled(Cancellation))
					{
						bOutCancelled = true;
						return RejectOperation(ECookPublishOperationError::CancelledStaging, Stage, Root / Relative);
					}
					if (Injected(Stage, Index)) return false;
					const std::filesystem::path Staged = StagedRoot / Relative;
					std::filesystem::create_directories(Staged.parent_path(), ErrorCode);
					if (ErrorCode || !FFileHelper::SaveArrayToFile(Bytes, Staged))
						return RejectOperation(ECookPublishOperationError::StageWrite, Stage, Staged, ErrorCode);
					FByteBuffer Validation;
					if (!FFileHelper::LoadFileToArray(Validation, Staged)
						|| !std::ranges::equal(Validation, Bytes))
						return RejectOperation(ECookPublishOperationError::StageValidation, Stage, Staged);
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
						return RejectOperation(ECookPublishOperationError::CancelledCommit, Stage, Root / Relative);
					}
					if (Injected(Stage, Index)) return false;
					const std::filesystem::path Destination = Root / Relative;
					const std::filesystem::path Staged = StagedRoot / Relative;
					const std::filesystem::path Backup = BackupRoot / Relative;
					std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
					if (ErrorCode) return RejectOperation(ECookPublishOperationError::CommitDirectory, Stage, Destination.parent_path(), ErrorCode);
					const bool bExists = std::filesystem::exists(Destination, ErrorCode) && !ErrorCode;
					if (bExists)
					{
						std::filesystem::create_directories(Backup.parent_path(), ErrorCode);
						std::filesystem::rename(Destination, Backup, ErrorCode);
						if (ErrorCode) return RejectOperation(ECookPublishOperationError::CommitBackup, Stage, Destination, ErrorCode);
					}
					Committed.push_back({Destination, Backup, bExists});
					std::filesystem::rename(Staged, Destination, ErrorCode);
					if (ErrorCode) return RejectOperation(ECookPublishOperationError::CommitReplace, Stage, Destination, ErrorCode);
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
						std::string Ignored;
						if (ShouldFail && ShouldFail(ECookOperationStage::StaleCleanup, Index, Ignored)) break;
						const std::filesystem::path Candidate = (Root / Entry.RelativePath).lexically_normal();
						const std::filesystem::path Relative = Candidate.lexically_relative(Root);
						if (Relative.empty() || Relative.native().starts_with(std::filesystem::path("..").native())) continue;
						std::filesystem::remove(Candidate, ErrorCode);
					}
				}
				InOutResult.CommitTimeNanoseconds += std::chrono::duration_cast<
														 std::chrono::nanoseconds>(std::chrono::steady_clock::now() - CommitStart)
														 .count();
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
