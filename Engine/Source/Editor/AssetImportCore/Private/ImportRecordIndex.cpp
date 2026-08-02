#include "ImportRecordIndex.h"

#include "AssetSystem.h"
#include "DObject/DObjectGlobals.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"

namespace Durin::AssetImport
{
	namespace
	{
		auto RegisterImportRecordMoveContributor() -> void;
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

	FImportRecordIndex::FImportRecordIndex() : Impl(std::make_unique<FImpl>())
	{
		RegisterImportRecordMoveContributor();
	}
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
		const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAsset(Path);
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
					const Asset::FAssetData* OutputData =
						Asset::GetAssetRegistry().FindAsset(Output.AssetPath);
					Management.bOutputMissing = OutputData == nullptr;
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
						Management.bFingerprintMismatch =
							!ComputePersistedImportPackageFingerprint(
								Output.AssetPath, PersistedFingerprint, FingerprintError)
							|| PersistedFingerprint != Output.AuthoredFingerprint;
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

	namespace
	{
		struct FMoveRecordEdit
		{
			DImportRecord* Record = nullptr;
			FImportRecordState Before;
			FImportRecordState After;
			bool bWasDirty = false;
		};

		auto RegisterImportRecordMoveContributor() -> void
		{
			static const bool Registered = [] {
				Asset::RegisterAssetMoveContributor(DObject::StaticClass(), [](
				DObject* MovingAsset,
				const FAssetPath& OldPath,
				const FAssetPath& NewPath,
				Asset::FAssetMoveContribution& Out) -> Asset::FAssetResult
			{
				if (!MovingAsset) return {};
				if (Cast<DImportRecord>(MovingAsset))
				{
					Out.Apply = [] { GetImportRecordIndex().NotifyAssetDuplicated(); };
					Out.Rollback = [] { GetImportRecordIndex().NotifyAssetDuplicated(); };
					return {};
				}
				std::string Error;
				FImportRecordIndex& Index = GetImportRecordIndex();
				if (!Index.EnsureCurrent(Error))
					return {Asset::EAssetError::InvalidObjectGraph, std::move(Error)};
				const std::vector<FImportRecordManagement> Managers = Index.FindManagers(OldPath);
				if (Managers.empty()) return {};
				if (Managers.size() != 1 || Index.IsRecordConflicted(Managers.front().RecordPath))
					return {Asset::EAssetError::InvalidObjectGraph,
						"A conflicted import-record manager prevents this output move."};

				auto Edits = std::make_shared<std::vector<FMoveRecordEdit>>();
				std::unordered_set<FAssetPath> SeenRecords;
				for (const FImportRecordManagement& Manager : Managers)
				{
					if (!SeenRecords.insert(Manager.RecordPath).second) continue;
					DImportRecord* Record = nullptr;
					const Asset::FAssetResult Load = Asset::LoadAsset(Manager.RecordPath, Record);
					if (!Load || !Record)
						return Load ? Asset::FAssetResult{Asset::EAssetError::InvalidObjectGraph,
							"Import-record manager could not be loaded."} : Load;
					FMoveRecordEdit Edit{
						.Record = Record,
						.Before = Record->GetState(),
						.After = Record->GetState(),
						.bWasDirty = Record->GetPackage()->IsDirty()};
					const auto Output = std::ranges::find(
						Edit.After.Outputs, OldPath, &FImportRecordOutput::AssetPath);
					if (Output == Edit.After.Outputs.end())
						return {Asset::EAssetError::InvalidObjectGraph,
							"Import-record manager does not contain the moved output."};
					Output->AssetPath = NewPath;
					if (Edit.After.PrimaryOutput == OldPath) Edit.After.PrimaryOutput = NewPath;
					std::string ValidationError;
					if (!Record->SetState(Edit.After, ValidationError))
						return {Asset::EAssetError::InvalidObjectGraph, std::move(ValidationError)};
					Record->SetState(Edit.Before, ValidationError);
					if (!Edit.bWasDirty) Record->GetPackage()->ClearDirty();
					Out.AdditionalPackages.push_back(Record->GetPackage());
					Edits->push_back(std::move(Edit));
				}
				Out.Apply = [Edits, MovingAsset, NewPath] {
					std::string AuthoredFingerprint;
					std::string FingerprintError;
					check(ComputeImportPackageFingerprint(
						MovingAsset->GetPackage(), AuthoredFingerprint, FingerprintError));
					for (FMoveRecordEdit& Edit : *Edits)
					{
						const auto Output = std::ranges::find(
							Edit.After.Outputs, NewPath, &FImportRecordOutput::AssetPath);
						check(Output != Edit.After.Outputs.end());
						Output->AuthoredFingerprint = AuthoredFingerprint;
						std::string Error;
						check(Edit.Record->SetState(Edit.After, Error));
					}
					GetImportRecordIndex().NotifyAssetDuplicated();
				};
				Out.Rollback = [Edits] {
					for (FMoveRecordEdit& Edit : *Edits)
					{
						std::string Error;
						check(Edit.Record->SetState(Edit.Before, Error));
						if (!Edit.bWasDirty) Edit.Record->GetPackage()->ClearDirty();
					}
					GetImportRecordIndex().NotifyAssetDuplicated();
				};
				return {};
				});
				return true;
			}();
			(void)Registered;
		}
	}
}
