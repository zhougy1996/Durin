#include "Asset/Cook.h"
#include "Shader/ShaderBuildProvider.h"

#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "Asset/References.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/References.h"
#include "DObject/Class.h"
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
		constexpr uint32 CookStateMagic = 0x54414e53; // SNAT
		constexpr uint32 CookStateVersion = 1;
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
			if (Value.empty() || Value.size() > 4096
				|| Value.size() > std::numeric_limits<uint32>::max()) return false;
			Writer.WriteU32(static_cast<uint32>(Value.size()));
			Writer.WriteBytes(std::as_bytes(std::span(Value)));
			return !Writer.HasError();
		}

		class FStateReader
		{
		public:
			explicit FStateReader(std::span<const std::byte> InBytes)
				: Reader(InBytes, {MaximumCookStateBytes, 4096})
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
				std::span<const std::byte> Encoded;
				if (!Reader.ReadRegion(Encoded, Size, 4096)) return false;
				OutValue.assign(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
				return OutValue.find('\0') == std::string::npos;
			}

			auto IsAtEnd() const -> bool { return Reader.IsAtEnd(); }

		private:
			FBinaryReader Reader;
		};

		struct FRegisteredCookContributor
		{
			FCookContributorHandle Handle = 0;
			FCookContributorRegistration Registration;
			FModuleOwnedResourceLease OwnerResource;
			FModuleOwnedCallbackGate OwnerGate;
		};

		auto GetCookContributorMutex() -> std::mutex&
		{
			static std::mutex Mutex;
			return Mutex;
		}

		auto GetCookContributors()
			-> std::unordered_map<DClass*, FRegisteredCookContributor>&
		{
			static std::unordered_map<DClass*, FRegisteredCookContributor> Contributors;
			return Contributors;
		}

		auto GetNextCookContributorHandle() -> FCookContributorHandle&
		{
			static FCookContributorHandle Handle = 1;
			return Handle;
		}

		struct FResolvedCookContributor
		{
			FCookContributorRegistration Registration;
			FModuleOwnedCallbackGate OwnerGate;
		};

		auto ResolveCookContributor(DClass* Class, FResolvedCookContributor& OutContributor) -> FAssetResult
		{
			OutContributor = {};
			std::scoped_lock Lock(GetCookContributorMutex());
			for (DClass* Candidate = Class; Candidate; Candidate = Candidate->GetSuperClass())
			{
				const auto Found = GetCookContributors().find(Candidate);
				if (Found == GetCookContributors().end()) continue;
				OutContributor.Registration = Found->second.Registration;
				OutContributor.OwnerGate = Found->second.OwnerGate;
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

		auto ValidateCookHit(const std::filesystem::path& Root, const FCookStateEntry& Entry) -> bool
		{
			if (!ValidateExistingFile(Root / RelativePackagePath(Entry.VirtualPackagePath), Entry.PackageSize, Entry.PackageDigest)) return false;
			return Entry.SegmentSize == 0
				   || ValidateExistingFile(Root / RelativeSegmentPath(Entry.VirtualPackagePath), Entry.SegmentSize, Entry.SegmentDigest);
		}

		auto BuildInputFingerprint(const FAssetData& Data, const FAssetRegistrySnapshot& Registry, const FCookRequest& Request, const FCookContributorRegistration& Contributor, FXxHash128& OutFingerprint, std::string& OutError) -> bool
		{
			FXxHash128 SourceDigest;
			std::error_code ErrorCode;
			if (!FFileHelper::HashFileXx128(Data.PhysicalPath, SourceDigest, ErrorCode))
				return CookFail(std::format("CookFingerprintReadFailed: {} ({})", Data.PackagePath.ToString(), ErrorCode.message()), &OutError);
			FXxHash128Builder Builder;
			constexpr uint32 FingerprintVersion = 1;
			Builder.UpdateValue(FingerprintVersion);
			Builder.Update(Data.PackagePath.GetView());
			Builder.UpdateValue(SourceDigest.HashLow);
			Builder.UpdateValue(SourceDigest.HashHigh);
			Builder.UpdateValue(static_cast<uint32>(Request.TargetPlatform));
			Builder.UpdateValue(static_cast<uint32>(Request.TargetProfile));
			Builder.UpdateValue(Contributor.ContributorVersion);
			Builder.UpdateValue(Contributor.FamilyProducerVersion);
			Builder.Update(Contributor.Name);
			struct FReferenceFact
			{
				FPackagePath Target;
				std::string ExpectedClass;
				std::string Route;
				EAssetReferenceKind Kind = EAssetReferenceKind::HardObject;
			};
			std::vector<FReferenceFact> Facts;
			for (const FAssetPackageReferenceEdge& Edge : Registry.References.GetEdges())
			{
				if (Edge.SourcePackage != Data.PackagePath
					|| Edge.Kind == EAssetReferenceKind::Redirect) continue;
				const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(
					Edge.TargetPath
				);
				if (!Resolution) return CookFail(std::format("CookFingerprintReferenceResolutionFailed: {}", Edge.TargetPath.ToString()), &OutError);
				Facts.push_back({Resolution.FinalPath, {}, "package dependency", Edge.Kind});
			}
			for (const FPackagePath& Dependency : Data.Dependencies)
			{
				const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(Dependency);
				if (!Resolution) return CookFail(std::format("CookFingerprintDependencyResolutionFailed: {}", Dependency.ToString()), &OutError);
				Facts.push_back({Resolution.FinalPath, {}, "package dependency", EAssetReferenceKind::HardObject});
			}
			std::ranges::sort(Facts, [](const FReferenceFact& Left, const FReferenceFact& Right) {
				return std::tuple{Left.Target.GetView(), Left.Kind, Left.ExpectedClass, Left.Route}
					   < std::tuple{Right.Target.GetView(), Right.Kind, Right.ExpectedClass, Right.Route};
			});
			Facts.erase(std::unique(Facts.begin(), Facts.end(), [](const FReferenceFact& Left, const FReferenceFact& Right) {
							return Left.Target == Right.Target && Left.Kind == Right.Kind
								   && Left.ExpectedClass == Right.ExpectedClass
								   && Left.Route == Right.Route;
						}),
						Facts.end());
			for (const FReferenceFact& Fact : Facts)
			{
				Builder.Update(Fact.Target.GetView());
				Builder.UpdateValue(static_cast<uint8>(Fact.Kind));
				Builder.Update(Fact.ExpectedClass);
				Builder.Update(Fact.Route);
				const auto Found = Registry.References.GetSourceFingerprints().find(
					Fact.Target
				);
				if (Found != Registry.References.GetSourceFingerprints().end())
				{
					Builder.UpdateValue(Found->second.ContentHash.HashLow);
					Builder.UpdateValue(Found->second.ContentHash.HashHigh);
				}
			}
			Builder.UpdateValue(static_cast<uint8>(Request.bRetainEditorOnlyData));
			OutFingerprint = Builder.Finalize();
			OutError.clear();
			return true;
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
			std::span<const std::byte> Bytes;
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

			auto Publish(std::span<const FCookSavePlan> Plans, std::span<const FCookAuxiliaryOutput> AuxiliaryOutputs, const FCookState& State, FCookRunResult& InOutResult, const FCookCancellationCheck& Cancellation, const FCookFailureInjection& ShouldFail, std::string& OutError) -> bool override
			{
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
				FByteArray PreviousManifestBytes;
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
				FByteArray ManifestBytes;
				FByteArray StateBytes;
				if (!EncodeCookManifest(Manifest, ManifestBytes, &OutError)
					|| !EncodeCookState(State, StateBytes, &OutError)) return false;

				auto StageBytes = [&](std::string_view Relative,
									  std::span<const std::byte> Bytes, ECookOperationStage Stage,
									  size_t Index) -> bool {
					if (IsCancelled(Cancellation)) return CookFail("CookCancelledDuringStaging", &OutError);
					if (ShouldFail && ShouldFail(Stage, Index, OutError)) return false;
					const std::filesystem::path Staged = StagedRoot / Relative;
					std::filesystem::create_directories(Staged.parent_path(), ErrorCode);
					if (ErrorCode || !FFileHelper::SaveArrayToFile(Bytes, Staged))
						return CookFail(std::format("CookStageWriteFailed: {}", Relative), &OutError);
					FByteArray Validation;
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
						return CookFail("CookCancelledDuringCommit", &OutError);
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

		private:
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

	auto EncodeCookState(const FCookState& State, FByteArray& OutBytes, std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (State.TargetPlatform == ECookTargetPlatform::Invalid
			|| State.TargetProfile == ECookTargetProfile::Invalid
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
		FBinaryWriter Writer({MaximumCookStateBytes, 4096});
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
		}
		if (Writer.HasError())
			return CookFail("Cook state exceeds its byte bound.", OutError);
		OutBytes = Writer.TakeBytes();
		if (OutError) OutError->clear();
		return true;
	}

	auto DecodeCookState(std::span<const std::byte> Bytes, FCookState& OutState, std::string* OutError) -> bool
	{
		OutState = {};
		if (Bytes.size() > MaximumCookStateBytes)
			return CookFail("Cook state exceeds its byte bound.", OutError);
		FStateReader Reader(Bytes);
		uint32 Magic = 0, Version = 0, Platform = 0, Profile = 0, Count = 0, Reserved = 0;
		if (!Reader.Read(Magic) || !Reader.Read(Version) || !Reader.Read(Platform)
			|| !Reader.Read(Profile) || !Reader.Read(Count) || !Reader.Read(Reserved)
			|| Magic != CookStateMagic || Version != CookStateVersion || Reserved != 0
			|| Count > MaximumCookStateEntries
			|| Platform == static_cast<uint32>(ECookTargetPlatform::Invalid)
			|| Profile == static_cast<uint32>(ECookTargetProfile::Invalid))
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

	auto RegisterCookContributor(DClass* Class, FCookContributorRegistration Registration, FModuleOwnedCallbackGate OwnerGate) -> FCookContributorHandle
	{
		auto Invocation = OwnerGate.TryEnter();
		if (!Class || Registration.Name.empty() || !Registration.Contribute
			|| Registration.ContributorVersion == 0
			|| Registration.FamilyProducerVersion == 0
			|| (OwnerGate.IsValid() && !Invocation)) return 0;
		std::scoped_lock Lock(GetCookContributorMutex());
		auto& Contributors = GetCookContributors();
		if (Contributors.contains(Class)) return 0;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		const FCookContributorHandle Handle = GetNextCookContributorHandle()++;
		Contributors.emplace(Class, FRegisteredCookContributor{Handle, std::move(Registration), std::move(Resource), std::move(OwnerGate)});
		return Handle;
	}

	auto UnregisterCookContributor(FCookContributorHandle Handle) -> void
	{
		if (Handle == 0) return;
		std::scoped_lock Lock(GetCookContributorMutex());
		std::erase_if(GetCookContributors(), [Handle](const auto& Pair) {
			return Pair.second.Handle == Handle;
		});
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
		const FAssetRegistrySnapshot Registry = CaptureAssetRegistrySnapshot();
		std::vector<FPackagePath> Packages;
		const FAssetResult Reachability = BuildCookReachability(
			Registry, Roots, Packages
		);
		if (!Reachability)
			return Finish(ECookRunStatus::Failed, "discovery-failed", Reachability.Message);
		if (Packages.empty())
			return Finish(ECookRunStatus::Failed, "empty-root-set", "CookEmptyRootSet: explicit and registered Cook roots selected no packages.");
		const FAssetCatalogSnapshot& Catalog = Registry.Catalog;

		FCookState PreviousState;
		bool bHasPreviousState = false;
		if (Request.IncrementalPolicy == ECookIncrementalPolicy::Enabled
			&& !Request.OutputRoot.empty())
		{
			FByteArray Bytes;
			bHasPreviousState = FFileHelper::LoadFileToArray(
									Bytes, Request.OutputRoot / "CookState.bin"
								)
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
		uint64 CapturedBytes = 0;
		for (size_t Index = 0; Index < Packages.size(); ++Index)
		{
			if (IsCancelled(Request.IsCancelled))
				return Finish(ECookRunStatus::Cancelled, "cancelled", "CookCancelledBeforePackagePreparation");
			const FPackagePath& Path = Packages[Index];
			if (Request.ReportProgress) Request.ReportProgress({ECookOperationStage::Load, Path, Index, Packages.size()});
			const FAssetData* Data = Catalog.FindExact(Path);
			if (!Data) return Finish(ECookRunStatus::Failed, "stale-registry", std::format("CookStaleRegistry: {} disappeared from the captured catalog.", Path.ToString()));
			if (Data->TopLevelAssets.empty())
				return Finish(ECookRunStatus::Failed, "missing-top-level-asset",
					std::format("CookMissingTopLevelAsset: {} has no independently addressable asset.",
						Path.ToString()));
			const FTopLevelAssetData& CookRoot = Data->TopLevelAssets.front();
			DClass* AssetClass = FindClassByQualifiedName(
				FName(CookRoot.AssetClassName));
			FResolvedCookContributor Contributor;
			const FAssetResult Resolution = ResolveCookContributor(AssetClass, Contributor);
			if (!Resolution)
			{
				OutResult.Packages.push_back({{}, Path, {}, "unsupported-class", Resolution.Message, ECookPackageStatus::Unsupported, ECookOperationStage::Prepare});
				return Finish(ECookRunStatus::Failed, "unsupported-class", Resolution.Message);
			}
			FXxHash128 Fingerprint;
			std::string Error;
			if (!BuildInputFingerprint(*Data, Registry, Request, Contributor.Registration, Fingerprint, Error))
				return Finish(ECookRunStatus::Failed, "fingerprint-failed", Error);
			const auto Prior = PriorEntries.find(Path.ToString());
			bool bCookHit = Prior != PriorEntries.end()
							&& Prior->second->InputFingerprint == Fingerprint
							&& Prior->second->ContributorVersion
								   == Contributor.Registration.ContributorVersion
							&& Prior->second->FamilyProducerVersion
								   == Contributor.Registration.FamilyProducerVersion
							&& ValidateCookHit(Request.OutputRoot, *Prior->second);
			FByteArray ExistingPackageBytes;
			FByteArray ExistingSegmentBytes;
			if (bCookHit)
			{
				bCookHit = FFileHelper::LoadFileToArray(ExistingPackageBytes, Request.OutputRoot / RelativePackagePath(Path.GetView()));
				if (bCookHit && Prior->second->SegmentSize != 0)
					bCookHit = FFileHelper::LoadFileToArray(ExistingSegmentBytes, Request.OutputRoot / RelativeSegmentPath(Path.GetView()));
			}
			if (bCookHit)
			{
				const FCookStateEntry& Hit = *Prior->second;
				Plans.push_back({.VirtualPath = Hit.VirtualPackagePath, .PackageBytes = std::move(ExistingPackageBytes), .BulkBytes = std::move(ExistingSegmentBytes), .InputFingerprint = Hit.InputFingerprint, .PackageDigest = Hit.PackageDigest, .SegmentDigest = Hit.SegmentDigest, .PackageFileSize = Hit.PackageSize, .SegmentFileSize = Hit.SegmentSize, .TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile, .ContributorVersion = Hit.ContributorVersion, .FamilyProducerVersion = Hit.FamilyProducerVersion, .Contributor = Hit.Contributor, .BuildProvenance = Hit.BuildProvenance, .bRawBulkSegment = (Hit.SegmentFlags & CookStateSegmentRawFieldProjection) != 0, .bOpaqueRawSegment = (Hit.SegmentFlags & CookStateSegmentOpaque) != 0, .bReuseExistingOutput = true});
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
			const FAssetResult LoadResult = LoadObject(CookRootPath, nullptr, Asset);
			if (!LoadResult || !Asset)
				return Finish(ECookRunStatus::Failed, "load-failed", std::format("CookLoadFailed: {}: {}", Path.ToString(), LoadResult.Message));
			FCookContext Capture({}, Request.TargetPlatform, Request.TargetProfile, Request.bRetainEditorOnlyData);
			auto Invocation = Contributor.OwnerGate.TryEnter();
			if (Contributor.OwnerGate.IsValid() && !Invocation)
				return Finish(ECookRunStatus::Failed, "contributor-retired", std::format("CookContributorRetired: {}", Contributor.Registration.Name));
			if (ShouldFail && ShouldFail(ECookOperationStage::Prepare, Index, Error))
				return Finish(ECookRunStatus::Failed, "prepare-injected-failure", Error);
			DPackage* AuthoredPackage = Asset->GetPackage();
			if (!AuthoredPackage)
				return Finish(ECookRunStatus::Failed, "missing-package", std::format("CookMissingPackage: package={}, contributor={}", Path.ToString(), Contributor.Registration.Name));
			const bool bWasDirty = AuthoredPackage->IsDirty();
			FByteArray AuthoredBytesBefore;
			const FAssetResult BeforeResult = SerializeAssetPackageBytes(
				AuthoredPackage, AuthoredBytesBefore
			);
			if (!BeforeResult)
				return Finish(ECookRunStatus::Failed, "authored-snapshot-failed", std::format("CookAuthoredSnapshotFailed: package={}, contributor={}: {}", Path.ToString(), Contributor.Registration.Name, BeforeResult.Message));
			const FAssetResult Contribution = Contributor.Registration.Contribute(
				*Asset, Path.GetView(), Capture
			);
			if (!Contribution)
				return Finish(ECookRunStatus::Failed, "contribution-failed", std::format("CookContributionFailed: package={}, contributor={}, stage=prepare: {}", Path.ToString(), Contributor.Registration.Name, Contribution.Message));
			FByteArray AuthoredBytesAfter;
			const FAssetResult AfterResult = SerializeAssetPackageBytes(
				AuthoredPackage, AuthoredBytesAfter
			);
			if (!AfterResult || AuthoredPackage->IsDirty() != bWasDirty
				|| AuthoredBytesAfter != AuthoredBytesBefore)
				return Finish(ECookRunStatus::Failed, "contributor-mutated-authored-package", std::format("CookContributorMutatedAuthoredPackage: package={}, contributor={}", Path.ToString(), Contributor.Registration.Name));
			if (ShouldFail && ShouldFail(ECookOperationStage::Capture, Index, Error))
				return Finish(ECookRunStatus::Failed, "capture-injected-failure", Error);
			std::vector<FCookSavePlan> Captured;
			if (!Capture.TakeSavePlans(Captured, &Error) || Captured.size() != 1)
				return Finish(ECookRunStatus::Failed, "capture-failed", std::format("CookCaptureFailed: package={}, contributor={}: {}", Path.ToString(), Contributor.Registration.Name, Error));
			FCookSavePlan Plan = std::move(Captured.front());
			Plan.InputFingerprint = Fingerprint;
			Plan.Contributor = Contributor.Registration.Name;
			Plan.ContributorVersion = Contributor.Registration.ContributorVersion;
			Plan.FamilyProducerVersion = Contributor.Registration.FamilyProducerVersion;
			const ECookPackageStatus PreparationStatus =
				Contributor.Registration.ClassifyPreparation ? Contributor.Registration.ClassifyPreparation(*Asset) : ECookPackageStatus::Captured;
			Plan.BuildProvenance = CookPackageStatusName(PreparationStatus);
			CapturedBytes += Plan.PackageFileSize + Plan.SegmentFileSize;
			OutResult.PeakCapturedBytes = std::max(OutResult.PeakCapturedBytes, CapturedBytes);
			OutResult.ChangedBytes += Plan.PackageFileSize + Plan.SegmentFileSize;
			NewState.Entries.push_back({Plan.VirtualPath, Plan.InputFingerprint, Plan.PackageDigest, Plan.SegmentDigest, Plan.PackageFileSize, Plan.SegmentFileSize, Plan.ContributorVersion, Plan.FamilyProducerVersion, Plan.Contributor, Plan.BuildProvenance});
			NewState.Entries.back().SegmentFlags = static_cast<uint8>(
				(Plan.bRawBulkSegment ? CookStateSegmentRawFieldProjection : 0)
				| (Plan.bOpaqueRawSegment ? CookStateSegmentOpaque : 0)
			);
			OutResult.Packages.push_back({{}, Path, Plan.Contributor, std::string(CookPackageStatusName(PreparationStatus)), "Captured deterministic package save plan.", PreparationStatus, ECookOperationStage::Capture, Plan.PackageFileSize, Plan.SegmentFileSize});
			Plans.push_back(std::move(Plan));
		}
		std::ranges::sort(Plans, {}, &FCookSavePlan::VirtualPath);
		std::ranges::sort(NewState.Entries, {}, &FCookStateEntry::VirtualPackagePath);
		if (Request.bDryRun)
			return Finish(ECookRunStatus::Succeeded, "dry-run", "Cook dry-run captured the complete plan.");
		std::vector<FCookAuxiliaryOutput> AuxiliaryOutputs;
		FByteArray ShaderLibraryBytes;
		std::string ShaderLibraryError;
		if (!BuildCookedShaderLibrary(
				EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
				ShaderLibraryBytes, ShaderLibraryError))
			return Finish(ECookRunStatus::Failed, "shader-library-failed",
				std::format("CookShaderLibraryFailed: {}", ShaderLibraryError));
		AuxiliaryOutputs.push_back({
			ECookManifestEntryKind::ShaderLibrary,
			std::string(ShaderCookedLibraryRelativePath),
			std::move(ShaderLibraryBytes)});
		AuxiliaryOutputs.back().Digest = FXxHash128::HashBuffer(AuxiliaryOutputs.back().Bytes);
		std::unique_ptr<ICookOutputStore> OwnedStore;
		if (!OutputStore)
		{
			OwnedStore = CreateLocalLooseCookOutputStore(Request.OutputRoot, Request.TargetPlatform, Request.TargetProfile);
			OutputStore = OwnedStore.get();
		}
		std::string PublishError;
		if (!OutputStore->Publish(Plans, AuxiliaryOutputs, NewState, OutResult, Request.IsCancelled, ShouldFail, PublishError))
		{
			if (IsCancelled(Request.IsCancelled)
				|| PublishError.starts_with("CookCancelled"))
				return Finish(ECookRunStatus::Cancelled, "cancelled", PublishError);
			return Finish(ECookRunStatus::Failed, "publication-failed", PublishError);
		}
		return Finish(ECookRunStatus::Succeeded, "succeeded", "Cook published a validated manifest-last output generation.");
	}
} // namespace Durin
