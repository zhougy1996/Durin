#include "ImportRecordIndex.h"

#include "AssetSystem.h"
#include "DObject/DObjectGlobals.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"

namespace Durin::Asset::Import
{
	namespace
	{
		constexpr std::string_view ImportRecordReferenceStoreId =
			"Durin.AssetImport.ImportRecords";
		constexpr uint64 ImportRecordReferenceStoreVersion = 1;

		enum class EImportRecordReferenceKind : uint8
		{
			Output,
			DetachedTombstone,
			PrimaryOutput,
		};

		struct FImportRecordReferenceDescriptor
		{
			DImportRecord* Record = nullptr;
			FAssetPath RecordPath;
			EImportRecordReferenceKind Kind = EImportRecordReferenceKind::Output;
			std::string Identity;
			FAssetPath TargetPath;
			std::string StableId;
		};

		struct FPreparedImportRecordRewrite
		{
			DImportRecord* Record = nullptr;
			FAssetPath RecordPath;
			FImportRecordState PreState;
			FImportRecordState PostState;
		};

		auto ImportStoreError(Asset::EAssetError Error, std::string Message)
			-> Asset::FAssetResult
		{
			return {Error, std::move(Message)};
		}

		auto MakeImportRecordReferenceStableId(
			const FAssetPath& RecordPath,
			std::string_view Category,
			std::string_view Identity) -> std::string
		{
			return std::format("{}::{}::{}", RecordPath.ToString(), Category, Identity);
		}

		auto CaptureImportRecordReferenceState(
			Asset::FAssetReferenceStoreSnapshot& OutSnapshot,
			std::vector<FImportRecordReferenceDescriptor>* OutDescriptors = nullptr)
			-> Asset::FAssetResult
		{
			OutSnapshot = {
				.ProviderId = std::string(ImportRecordReferenceStoreId),
				.ProviderVersion = ImportRecordReferenceStoreVersion};
			if (OutDescriptors) OutDescriptors->clear();

			std::vector<FAssetPath> RecordPaths;
			constexpr std::string_view RecordClass =
				"Durin::AssetImport::DImportRecord";
			for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
				if (Data.EntryKind == Asset::EAssetRegistryEntryKind::Asset
					&& Data.AssetClassName == RecordClass)
					RecordPaths.push_back(Path);
			std::ranges::sort(RecordPaths,
				[](const FAssetPath& Left, const FAssetPath& Right) {
					return Left.GetView() < Right.GetView();
				});

			std::string FingerprintSource = std::format(
				"{}\n{}\n", ImportRecordReferenceStoreId,
				ImportRecordReferenceStoreVersion);
			for (const FAssetPath& RecordPath : RecordPaths)
			{
				DImportRecord* Record = nullptr;
				Asset::FAssetLoadReport LoadReport;
				Asset::FAssetResult Result = Asset::LoadAsset(
					RecordPath, Record, &LoadReport);
				if (!Result || !Record)
					return Result ? ImportStoreError(
						Asset::EAssetError::InvalidObjectGraph,
						"An import record has no loaded main object.") : Result;
				if (LoadReport.HasRiskItems())
					return ImportStoreError(
						Asset::EAssetError::UnsupportedProperty,
						std::format("Import record {} has compatibility-risk fields.",
							RecordPath.ToString()));
				if (!Record->GetPackage() || Record->GetPackage()->IsDirty())
					return ImportStoreError(
						Asset::EAssetError::InUse,
						std::format("Dirty import record {} blocks redirector Fix Up.",
							RecordPath.ToString()));
				std::string ValidationError;
				if (!Record->Validate(ValidationError))
					return ImportStoreError(
						Asset::EAssetError::InvalidObjectGraph,
						std::format("Import record {} is invalid: {}",
							RecordPath.ToString(), ValidationError));
				std::string PersistedFingerprint;
				if (!ComputePersistedImportPackageFingerprint(
						RecordPath, PersistedFingerprint, ValidationError))
					return ImportStoreError(
						Asset::EAssetError::IoError, std::move(ValidationError));
				FingerprintSource += std::format(
					"{}\n{}\n", RecordPath.ToString(), PersistedFingerprint);

				auto AddOccurrence = [&](EImportRecordReferenceKind Kind,
					std::string_view Category, std::string_view Identity,
					const FAssetPath& TargetPath, std::string DisplayRoute) {
					FImportRecordReferenceDescriptor Descriptor{
						.Record = Record,
						.RecordPath = RecordPath,
						.Kind = Kind,
						.Identity = std::string(Identity),
						.TargetPath = TargetPath,
						.StableId = MakeImportRecordReferenceStableId(
							RecordPath, Category, Identity)};
					OutSnapshot.Occurrences.push_back({
						.ProviderId = std::string(ImportRecordReferenceStoreId),
						.StableId = Descriptor.StableId,
						.TargetPath = TargetPath,
						.DisplayRoute = std::move(DisplayRoute)});
					if (OutDescriptors)
						OutDescriptors->push_back(std::move(Descriptor));
				};

				for (const FImportRecordOutput& Output : Record->GetOutputs())
					AddOccurrence(
						EImportRecordReferenceKind::Output, "output",
						Output.StableIdentity, Output.AssetPath,
						std::format("{}.Outputs[{}].AssetPath",
							RecordPath.ToString(), Output.StableIdentity));
				for (const FImportRecordDetachedTombstone& Tombstone :
					Record->GetDetachedTombstones())
					AddOccurrence(
						EImportRecordReferenceKind::DetachedTombstone,
						"tombstone", Tombstone.StableIdentity,
						Tombstone.LastAssetPath,
						std::format("{}.DetachedTombstones[{}].LastAssetPath",
							RecordPath.ToString(), Tombstone.StableIdentity));
				if (Record->GetPrimaryOutput().IsValid())
					AddOccurrence(
						EImportRecordReferenceKind::PrimaryOutput,
						"primary", "root", Record->GetPrimaryOutput(),
						std::format("{}.PrimaryOutput", RecordPath.ToString()));
			}

			std::ranges::sort(OutSnapshot.Occurrences,
				[](const Asset::FAssetReferenceStoreOccurrence& Left,
					const Asset::FAssetReferenceStoreOccurrence& Right) {
					if (Left.StableId != Right.StableId)
						return Left.StableId < Right.StableId;
					return Left.TargetPath.GetView() < Right.TargetPath.GetView();
				});
			if (OutDescriptors)
				std::ranges::sort(*OutDescriptors,
					[](const FImportRecordReferenceDescriptor& Left,
						const FImportRecordReferenceDescriptor& Right) {
						return Left.StableId < Right.StableId;
					});
			OutSnapshot.Fingerprint = FXxHash128::HashBuffer(std::span{
				reinterpret_cast<const uint8*>(FingerprintSource.data()),
				FingerprintSource.size()}).ToString();
			return {};
		}
	}

	struct FImportRecordIndex::FImpl
	{
		mutable std::mutex Mutex;
		std::unordered_map<FAssetPath, std::vector<FImportRecordManagement>> ByOutput;
		std::unordered_map<FAssetPath, std::vector<FImportRecordManagement>> ByRecord;
		std::unordered_set<FAssetPath> ConflictedRecords;
		std::vector<FImportRecordIndexDiagnostic> Diagnostics;
		uint64 Revision = 1;
		uint64 ObservedAssetRegistryRevision = 0;
	};

	FImportRecordIndex::FImportRecordIndex() : Impl(std::make_unique<FImpl>()) {}
	FImportRecordIndex::~FImportRecordIndex() = default;

	auto ComputeImportPackageFingerprint(
		DPackage* Package,
		std::string& OutFingerprint,
		std::string& OutError) -> bool
	{
		std::vector<uint8> Bytes;
		const Asset::FAssetResult Result = Asset::SerializeAssetPackageBytes(Package, Bytes);
		if (!Result)
		{
			OutError = Result.Message;
			return false;
		}
		OutFingerprint = FXxHash128::HashBuffer(std::span<const uint8>(Bytes)).ToString();
		OutError.clear();
		return true;
	}

	auto ComputePersistedImportPackageFingerprint(
		const FAssetPath& Path,
		std::string& OutFingerprint,
		std::string& OutError) -> bool
	{
		const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAssetExact(Path);
		std::vector<uint8> Bytes;
		if (!Data || !FFileHelper::LoadFileToArray(Bytes, Data->PhysicalPath))
		{
			OutError = std::format("Authored package {} is unavailable.", Path.ToString());
			return false;
		}
		OutFingerprint = FXxHash128::HashBuffer(std::span<const uint8>(Bytes)).ToString();
		OutError.clear();
		return true;
	}

	auto FImportRecordIndex::Rebuild(std::string& OutError) -> bool
	{
		std::unordered_map<FAssetPath, std::vector<FImportRecordManagement>> ByOutput;
		std::unordered_map<FAssetPath, std::vector<FImportRecordManagement>> ByRecord;
		std::unordered_set<FAssetPath> ConflictedRecords;
		std::vector<FImportRecordIndexDiagnostic> Diagnostics;
		std::unordered_map<FGuid, std::vector<FAssetPath>> RecordsById;
		constexpr std::string_view RecordClass = "Durin::AssetImport::DImportRecord";

		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
		{
			if (Data.AssetClassName != RecordClass) continue;
			DImportRecord* Record = nullptr;
			const Asset::FAssetResult Load = Asset::LoadAsset(Path, Record);
			std::string ValidationError;
			if (!Load || !Record || !Record->Validate(ValidationError))
			{
				Diagnostics.push_back({
					.Category = EImportRecordIndexDiagnostic::InvalidRecord,
					.RecordPath = Path,
					.Message = Load ? ValidationError : Load.Message});
				continue;
			}
			RecordsById[Record->GetRecordId()].push_back(Path);
			for (const FImportRecordOutput& Output : Record->GetOutputs())
			{
				FImportRecordManagement Management{
					.RecordPath = Path,
					.RecordId = Record->GetRecordId(),
					.OutputIdentity = Output.StableIdentity,
					.OutputClassName = Output.AssetClassName,
					.AuthoredFingerprint = Output.AuthoredFingerprint,
					.Policy = Output.Policy};
				if (Output.Policy == EImportRecordOutputPolicy::Managed)
				{
					const Asset::FAssetPathResolveResult Resolution =
						Asset::GetAssetRegistry().ResolveAssetPath(Output.AssetPath);
					Management.bOutputMissing = !Resolution;
					if (Management.bOutputMissing)
						Diagnostics.push_back({
							.Category = EImportRecordIndexDiagnostic::MissingManagedOutput,
							.RecordPath = Path,
							.OutputPath = Output.AssetPath,
							.Message = std::format("Managed output {} is missing.", Output.AssetPath.ToString())});
					else
					{
						std::string PersistedFingerprint;
						std::string FingerprintError;
						// Relocation changes the package main-object identity while the
						// import record intentionally retains its authored alias.
						Management.bFingerprintMismatch =
							Resolution.RedirectChain.empty()
							&& (
							!ComputePersistedImportPackageFingerprint(
								Resolution.FinalPath, PersistedFingerprint, FingerprintError)
							|| PersistedFingerprint != Output.AuthoredFingerprint);
						if (Management.bFingerprintMismatch)
							Diagnostics.push_back({
								.Category = EImportRecordIndexDiagnostic::OutputFingerprintMismatch,
								.RecordPath = Path,
								.OutputPath = Output.AssetPath,
								.Message = std::format(
									"Managed output {} does not match the authoritative record fingerprint.",
									Output.AssetPath.ToString())});
					}
				}
				ByOutput[Output.AssetPath].push_back(Management);
				ByRecord[Path].push_back(std::move(Management));
			}
		}

		for (const auto& [RecordId, Paths] : RecordsById)
		{
			if (Paths.size() < 2) continue;
			for (const FAssetPath& Path : Paths)
			{
				ConflictedRecords.insert(Path);
				Diagnostics.push_back({
					.Category = EImportRecordIndexDiagnostic::DuplicateRecordId,
					.RecordPath = Path,
					.Message = std::format(
						"Import record identifier {} is duplicated by {} records.",
						RecordId.ToString(), Paths.size())});
			}
		}
		for (const auto& [OutputPath, Managers] : ByOutput)
		{
			const size_t ManagedCount = std::ranges::count(
				Managers, EImportRecordOutputPolicy::Managed, &FImportRecordManagement::Policy);
			if (ManagedCount < 2) continue;
			for (const FImportRecordManagement& Manager : Managers)
			{
				if (Manager.Policy != EImportRecordOutputPolicy::Managed) continue;
				ConflictedRecords.insert(Manager.RecordPath);
				Diagnostics.push_back({
					.Category = EImportRecordIndexDiagnostic::DuplicateManager,
					.RecordPath = Manager.RecordPath,
					.OutputPath = OutputPath,
					.Message = std::format(
						"Output {} is managed by {} import records.",
						OutputPath.ToString(), ManagedCount)});
			}
		}
		for (auto& [_, Values] : ByOutput)
			std::ranges::sort(Values, {}, [](const FImportRecordManagement& Value) {
				return Value.RecordPath.GetView();
			});

		std::lock_guard Lock(Impl->Mutex);
		Impl->ByOutput = std::move(ByOutput);
		Impl->ByRecord = std::move(ByRecord);
		Impl->ConflictedRecords = std::move(ConflictedRecords);
		Impl->Diagnostics = std::move(Diagnostics);
		Impl->ObservedAssetRegistryRevision = Asset::GetAssetRegistry().GetRevision();
		++Impl->Revision;
		OutError.clear();
		return true;
	}

	auto FImportRecordIndex::EnsureCurrent(std::string& OutError) -> bool
	{
		{
			std::lock_guard Lock(Impl->Mutex);
			if (Impl->ObservedAssetRegistryRevision == Asset::GetAssetRegistry().GetRevision())
			{
				OutError.clear();
				return true;
			}
		}
		return Rebuild(OutError);
	}

	auto FImportRecordIndex::FindManagers(const FAssetPath& OutputPath) const
		-> std::vector<FImportRecordManagement>
	{
		std::lock_guard Lock(Impl->Mutex);
		const auto It = Impl->ByOutput.find(OutputPath);
		if (It == Impl->ByOutput.end()) return {};
		std::vector<FImportRecordManagement> Result;
		std::ranges::copy_if(It->second, std::back_inserter(Result), [](const auto& Entry) {
			return Entry.Policy == EImportRecordOutputPolicy::Managed;
		});
		return Result;
	}

	auto FImportRecordIndex::FindRecordOutputs(const FAssetPath& RecordPath) const
		-> std::vector<FImportRecordManagement>
	{
		std::lock_guard Lock(Impl->Mutex);
		const auto It = Impl->ByRecord.find(RecordPath);
		return It == Impl->ByRecord.end() ? std::vector<FImportRecordManagement>{} : It->second;
	}

	auto FImportRecordIndex::IsRecordConflicted(const FAssetPath& RecordPath) const -> bool
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->ConflictedRecords.contains(RecordPath);
	}

	auto FImportRecordIndex::GetDiagnostics() const -> std::vector<FImportRecordIndexDiagnostic>
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Diagnostics;
	}

	auto FImportRecordIndex::GetRevision() const -> uint64
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Revision;
	}

	auto FImportRecordIndex::GetObservedAssetRegistryRevision() const -> uint64
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->ObservedAssetRegistryRevision;
	}

	auto FImportRecordIndex::ClearForProjectSwitch() -> void
	{
		std::lock_guard Lock(Impl->Mutex);
		Impl->ByOutput.clear();
		Impl->ByRecord.clear();
		Impl->ConflictedRecords.clear();
		Impl->Diagnostics.clear();
		Impl->ObservedAssetRegistryRevision = 0;
		++Impl->Revision;
	}

	auto FImportRecordIndex::NotifyAssetDeleted(const FAssetPath&) -> void
	{
		std::lock_guard Lock(Impl->Mutex);
		Impl->ObservedAssetRegistryRevision = 0;
	}

	auto FImportRecordIndex::NotifyAssetDuplicated() -> void
	{
		std::lock_guard Lock(Impl->Mutex);
		Impl->ObservedAssetRegistryRevision = 0;
	}

	auto FImportRecordIndex::NotifyPackageUnloaded(const FAssetPath&) -> void
	{
		// Summaries own only value types; package residency is intentionally irrelevant.
	}

	auto FImportRecordIndex::CaptureSnapshot(
		Asset::FAssetReferenceStoreSnapshot& OutSnapshot)
		-> Asset::FAssetResult
	{
		return CaptureImportRecordReferenceState(OutSnapshot);
	}

	auto FImportRecordIndex::PrepareRewrite(
		std::span<const Asset::FAssetReferenceRewrite> Rewrites,
		std::string_view ExpectedFingerprint,
		Asset::FAssetReferenceStoreRewriteContribution& OutContribution)
		-> Asset::FAssetResult
	{
		OutContribution = {};
		Asset::FAssetReferenceStoreSnapshot Snapshot;
		std::vector<FImportRecordReferenceDescriptor> Descriptors;
		Asset::FAssetResult Result = CaptureImportRecordReferenceState(
			Snapshot, &Descriptors);
		if (!Result) return Result;
		if (ExpectedFingerprint != Snapshot.Fingerprint)
			return ImportStoreError(
				Asset::EAssetError::StaleData,
				"Import records changed before Fix Up rewrite preparation.");

		std::unordered_map<std::string, const FImportRecordReferenceDescriptor*>
			ByStableId;
		for (const FImportRecordReferenceDescriptor& Descriptor : Descriptors)
			ByStableId.emplace(Descriptor.StableId, &Descriptor);
		std::map<FAssetPath, FPreparedImportRecordRewrite,
			decltype([](const FAssetPath& Left, const FAssetPath& Right) {
				return Left.GetView() < Right.GetView();
			})> PreparedByPath;
		std::unordered_set<std::string> SeenRewrites;
		for (const Asset::FAssetReferenceRewrite& Rewrite : Rewrites)
		{
			const auto Found = ByStableId.find(Rewrite.StableId);
			if (Found == ByStableId.end()
				|| !SeenRewrites.insert(Rewrite.StableId).second
				|| Found->second->TargetPath != Rewrite.SourcePath
				|| !Rewrite.DestinationPath.IsValid())
				return ImportStoreError(
					Asset::EAssetError::StaleData,
					"An import-record Fix Up rewrite no longer matches its occurrence.");
			const FImportRecordReferenceDescriptor& Descriptor = *Found->second;
			auto [Prepared, Inserted] = PreparedByPath.try_emplace(
				Descriptor.RecordPath);
			if (Inserted)
			{
				Prepared->second.Record = Descriptor.Record;
				Prepared->second.RecordPath = Descriptor.RecordPath;
				Prepared->second.PreState = Descriptor.Record->GetState();
				Prepared->second.PostState = Prepared->second.PreState;
			}
			FImportRecordState& State = Prepared->second.PostState;
			switch (Descriptor.Kind)
			{
			case EImportRecordReferenceKind::Output:
			{
				const auto Output = std::ranges::find(
					State.Outputs, Descriptor.Identity,
					&FImportRecordOutput::StableIdentity);
				if (Output == State.Outputs.end()
					|| Output->AssetPath != Rewrite.SourcePath)
					return ImportStoreError(
						Asset::EAssetError::StaleData,
						"An import-record output changed during Fix Up analysis.");
				Output->AssetPath = Rewrite.DestinationPath;
				break;
			}
			case EImportRecordReferenceKind::DetachedTombstone:
			{
				const auto Tombstone = std::ranges::find(
					State.DetachedTombstones, Descriptor.Identity,
					&FImportRecordDetachedTombstone::StableIdentity);
				if (Tombstone == State.DetachedTombstones.end()
					|| Tombstone->LastAssetPath != Rewrite.SourcePath)
					return ImportStoreError(
						Asset::EAssetError::StaleData,
						"An import-record tombstone changed during Fix Up analysis.");
				Tombstone->LastAssetPath = Rewrite.DestinationPath;
				break;
			}
			case EImportRecordReferenceKind::PrimaryOutput:
				if (State.PrimaryOutput != Rewrite.SourcePath)
					return ImportStoreError(
						Asset::EAssetError::StaleData,
						"An import-record primary output changed during Fix Up analysis.");
				State.PrimaryOutput = Rewrite.DestinationPath;
				break;
			}
		}

		auto Prepared = std::make_shared<std::vector<FPreparedImportRecordRewrite>>();
		Prepared->reserve(PreparedByPath.size());
		std::vector<Asset::FAssetReferenceStorePackageRewrite> PackageRewrites;
		PackageRewrites.reserve(PreparedByPath.size());
		for (auto& [RecordPath, RecordRewrite] : PreparedByPath)
		{
			DImportRecord* Record = RecordRewrite.Record;
			if (!Record || !Record->GetPackage() || Record->GetPackage()->IsDirty())
				return ImportStoreError(
					Asset::EAssetError::InUse,
					"A dirty import record blocks redirector Fix Up.");
			const Asset::FAssetData* Data =
				Asset::GetAssetRegistry().FindAssetExact(RecordPath);
			if (!Data)
				return ImportStoreError(
					Asset::EAssetError::StaleData,
					"An import record was removed during Fix Up analysis.");
			std::vector<uint8> PreBytes;
			if (!FFileHelper::LoadFileToArray(PreBytes, Data->PhysicalPath))
				return ImportStoreError(
					Asset::EAssetError::IoError,
					"Could not read an import record for Fix Up.");

			std::string StateError;
			if (!Record->SetState(RecordRewrite.PostState, StateError))
				return ImportStoreError(
					Asset::EAssetError::InvalidObjectGraph, std::move(StateError));
			RecordRewrite.PostState = Record->GetState();
			std::vector<uint8> PostBytes;
			Result = Asset::SerializeAssetPackageBytes(
				Record->GetPackage(), PostBytes);
			std::string RestoreError;
			const bool bRestored = Record->SetState(
				RecordRewrite.PreState, RestoreError);
			Record->GetPackage()->ClearDirty();
			if (!bRestored)
				return ImportStoreError(
					Asset::EAssetError::InvalidObjectGraph,
					std::format("Could not restore import record after Fix Up preparation: {}",
						RestoreError));
			if (!Result) return Result;
			PackageRewrites.push_back({
				.PackagePath = RecordPath,
				.PreBytes = std::move(PreBytes),
				.PostBytes = std::move(PostBytes)});
			Prepared->push_back(std::move(RecordRewrite));
		}

		OutContribution = {
			.Fingerprint = Snapshot.Fingerprint,
			.Rewrites = std::vector<Asset::FAssetReferenceRewrite>(
				Rewrites.begin(), Rewrites.end()),
			.PackageRewrites = std::move(PackageRewrites),
			.Revalidate = [Prepared] {
				for (const FPreparedImportRecordRewrite& Rewrite : *Prepared)
				{
					DImportRecord* Current = nullptr;
					const Asset::FAssetResult Load = Asset::LoadAsset(
						Rewrite.RecordPath, Current);
					if (!Load || Current != Rewrite.Record || !Current->GetPackage()
						|| Current->GetPackage()->IsDirty()
						|| Current->GetState() != Rewrite.PreState)
						return ImportStoreError(
							Asset::EAssetError::StaleData,
							"An import record changed after Fix Up analysis.");
				}
				return Asset::FAssetResult{};
			},
			.Apply = [this, Prepared] {
				size_t AppliedCount = 0;
				for (FPreparedImportRecordRewrite& Rewrite : *Prepared)
				{
					std::string Error;
					if (!Rewrite.Record->SetState(Rewrite.PostState, Error))
					{
						for (size_t Index = AppliedCount; Index > 0; --Index)
						{
							std::string Ignored;
							(*Prepared)[Index - 1].Record->SetState(
								(*Prepared)[Index - 1].PreState, Ignored);
							(*Prepared)[Index - 1].Record->GetPackage()->ClearDirty();
						}
						return ImportStoreError(
							Asset::EAssetError::InvalidObjectGraph,
							std::move(Error));
					}
					Rewrite.Record->GetPackage()->ClearDirty();
					++AppliedCount;
				}
				NotifyAssetDuplicated();
				return Asset::FAssetResult{};
			},
			.Restore = [this, Prepared] {
				for (auto It = Prepared->rbegin(); It != Prepared->rend(); ++It)
				{
					std::string Error;
					if (!It->Record->SetState(It->PreState, Error))
						return ImportStoreError(
							Asset::EAssetError::InvalidObjectGraph,
							std::move(Error));
					It->Record->GetPackage()->ClearDirty();
				}
				NotifyAssetDuplicated();
				return Asset::FAssetResult{};
			},
			.Verify = [Prepared] {
				for (const FPreparedImportRecordRewrite& Rewrite : *Prepared)
					if (!Rewrite.Record || !Rewrite.Record->GetPackage()
						|| Rewrite.Record->GetPackage()->IsDirty()
						|| Rewrite.Record->GetState() != Rewrite.PostState)
						return ImportStoreError(
							Asset::EAssetError::StaleData,
							"An import record did not retain its Fix Up rewrite.");
				return Asset::FAssetResult{};
			}};
		return {};
	}

	auto InspectImportRecord(
		const FAssetPath& RecordPath,
		FImportRecordIndex& Index) -> FImportRecordInspection
	{
		FImportRecordInspection Result{.RecordPath = RecordPath};
		if (!RecordPath.IsValid())
		{
			Result.Message = "Import-record inspection requires a valid record path.";
			return Result;
		}
		std::string Error;
		if (!Index.EnsureCurrent(Error))
		{
			Result.Message = std::move(Error);
			return Result;
		}
		const Asset::FAssetResult Load = Asset::LoadAsset(RecordPath, Result.Record);
		if (!Load || !Result.Record)
		{
			Result.Message = Load ? "The import record could not be loaded." : Load.Message;
			return Result;
		}
		Result.Outputs = Index.FindRecordOutputs(RecordPath);
		Result.bConflicted = Index.IsRecordConflicted(RecordPath);
		for (const FImportRecordManagement& Output : Result.Outputs)
		{
			Result.bHasMissingManagedOutput |= Output.bOutputMissing;
			Result.bHasFingerprintMismatch |= Output.bFingerprintMismatch;
		}
		for (const FImportRecordIndexDiagnostic& Diagnostic : Index.GetDiagnostics())
			if (Diagnostic.RecordPath == RecordPath)
				Result.Diagnostics.push_back(Diagnostic);
		Result.bSucceeded = true;
		return Result;
	}

	auto InspectImportRecordForOutput(
		const FAssetPath& OutputPath,
		FImportRecordIndex& Index) -> FImportRecordInspection
	{
		FImportRecordInspection Result{.SelectedOutputPath = OutputPath};
		if (!OutputPath.IsValid())
		{
			Result.Message = "Import-record navigation requires a valid output path.";
			return Result;
		}
		std::string Error;
		if (!Index.EnsureCurrent(Error))
		{
			Result.Message = std::move(Error);
			return Result;
		}
		const std::vector<FImportRecordManagement> Managers = Index.FindManagers(OutputPath);
		if (Managers.empty())
		{
			Result.Message = "The selected output is not managed by an import record.";
			return Result;
		}
		if (Managers.size() != 1)
		{
			Result.Message = "The selected output has multiple import-record managers.";
			return Result;
		}
		Result = InspectImportRecord(Managers.front().RecordPath, Index);
		Result.SelectedOutputPath = OutputPath;
		return Result;
	}

	auto DetachImportRecordOutput(
		DImportRecord& Record,
		std::string_view StableOutputIdentity,
		FImportRecordIndex& Index,
		const Asset::FAssetBundleSaveOptions& SaveOptions) -> FImportRecordEditResult
	{
		FImportRecordEditResult Result{.Record = &Record};
		if (!Record.GetPackage() || StableOutputIdentity.empty())
		{
			Result.Message = "Detach requires a packaged import record and output identity.";
			return Result;
		}
		FImportRecordState Before = Record.GetState();
		FImportRecordState After = Before;
		const auto Output = std::ranges::find(
			After.Outputs, StableOutputIdentity, &FImportRecordOutput::StableIdentity);
		if (Output == After.Outputs.end()
			|| Output->Policy != EImportRecordOutputPolicy::Managed)
		{
			Result.Message = "Only a currently managed output can be detached.";
			return Result;
		}
		Output->Policy = EImportRecordOutputPolicy::Detached;
		Result.RevealPath = Output->AssetPath;
		const bool bWasDirty = Record.GetPackage()->IsDirty();
		if (!Record.SetState(std::move(After), Result.Message)) return Result;
		DPackage* Package = Record.GetPackage();
		const Asset::FAssetResult Save = Asset::SavePackagesAtomically(
			std::span<DPackage* const>(&Package, 1), SaveOptions);
		if (!Save)
		{
			std::string RestoreError;
			Record.SetState(std::move(Before), RestoreError);
			if (!bWasDirty) Record.GetPackage()->ClearDirty();
			Result.Message = Save.Message;
			return Result;
		}
		Index.NotifyAssetDuplicated();
		std::string RebuildError;
		if (!Index.Rebuild(RebuildError))
		{
			Result.Message = std::move(RebuildError);
			return Result;
		}
		Result.bSucceeded = true;
		return Result;
	}

	auto RepairDuplicatedImportRecord(
		DImportRecord& Record,
		FImportRecordIndex& Index,
		const Asset::FAssetBundleSaveOptions& SaveOptions) -> FImportRecordEditResult
	{
		FImportRecordEditResult Result{.Record = &Record};
		if (!Record.GetPackage())
		{
			Result.Message = "Record repair requires a packaged import record.";
			return Result;
		}
		FAssetPath RecordPath;
		if (!FAssetPath::TryCreate(Record.GetPackage()->GetPackagePath(), RecordPath))
		{
			Result.Message = "Record repair could not resolve the package path.";
			return Result;
		}
		std::string Error;
		if (!Index.EnsureCurrent(Error) || !Index.IsRecordConflicted(RecordPath))
		{
			Result.Message = Error.empty()
				? "The selected import record has no identity or manager conflict." : Error;
			return Result;
		}
		const FGuid BeforeId = Record.GetRecordId();
		const FImportRecordState Before = Record.GetState();
		const bool bWasDirty = Record.GetPackage()->IsDirty();
		if (!Record.SetRecordIdForClone(FGuid::NewGuid(), Result.Message)) return Result;
		DPackage* Package = Record.GetPackage();
		const Asset::FAssetResult Save = Asset::SavePackagesAtomically(
			std::span<DPackage* const>(&Package, 1), SaveOptions);
		if (!Save)
		{
			std::string RestoreError;
			Record.SetRecordIdForClone(BeforeId, RestoreError);
			Record.SetState(Before, RestoreError);
			if (!bWasDirty) Package->ClearDirty();
			Result.Message = Save.Message;
			return Result;
		}
		Index.NotifyAssetDuplicated();
		if (!Index.Rebuild(Result.Message)) return Result;
		Result.RevealPath = RecordPath;
		Result.bSucceeded = true;
		return Result;
	}

	auto GetImportRecordIndex() -> FImportRecordIndex&
	{
		static FImportRecordIndex Index;
		return Index;
	}

}
