#include "ImportRecord.h"

#include "AssetImportCore.h"
#include "AssetSystem.h"
#include "Hash/XxHash.h"

namespace Durin::AssetImport
{
	namespace
	{
		constexpr size_t MaximumRecordStringBytes = 1024;

		auto IsIdentifier(std::string_view Value) -> bool
		{
			return !Value.empty() && Value.size() <= MaximumRecordStringBytes
				&& std::ranges::all_of(Value, [](unsigned char Character) {
					return std::isalnum(Character) || Character == '.' || Character == '_'
						|| Character == '-' || Character == ':' || Character == '/';
				});
		}

		auto ValidatePayload(
			const FImportRecordPayload& Payload,
			uint64 MaximumBytes,
			std::string_view Label,
			std::string& OutError) -> bool
		{
			if (!IsIdentifier(Payload.SchemaId) || Payload.SchemaVersion == 0
				|| Payload.Bytes.size() > MaximumBytes)
			{
				OutError = std::format("Import-record {} payload is invalid or exceeds its byte limit.", Label);
				return false;
			}
			const FXxHash128 Hash = FXxHash128::HashBuffer(std::span<const uint8>(Payload.Bytes));
			if (Hash.HashLow != Payload.ContentHashLow || Hash.HashHigh != Payload.ContentHashHigh)
			{
				OutError = std::format("Import-record {} payload hash does not match its bytes.", Label);
				return false;
			}
			return true;
		}

		auto ValidateState(const FImportRecordState& State, std::string& OutError) -> bool
		{
			if (!IsIdentifier(State.ProviderId) || State.ProviderContractVersion == 0)
			{
				OutError = "Import-record provider identity or contract version is invalid.";
				return false;
			}
			if (!ValidatePayload(State.Settings, MaximumImportRecordSettingsBytes, "settings", OutError)
				|| !ValidatePayload(State.ProviderState, MaximumImportRecordProviderStateBytes,
					"provider-state", OutError)) return false;
			if (State.Sources.empty() || State.Sources.size() > MaximumImportRecordSources
				|| State.Outputs.empty() || State.Outputs.size() > MaximumImportRecordOutputs
				|| State.DetachedTombstones.size() > MaximumImportRecordDetachedTombstones
				|| State.AcceptedDiagnostics.size() > MaximumImportRecordAcceptedDiagnostics)
			{
				OutError = "Import-record source, output, or detached-tombstone count is invalid.";
				return false;
			}

			std::unordered_set<std::string> SourceIdentities;
			for (const FImportRecordSource& Source : State.Sources)
			{
				if (!IsIdentifier(Source.StableIdentity) || Source.Role.empty()
					|| Source.Role.size() > MaximumRecordStringBytes || Source.SourcePath.IsEmpty()
					|| Source.ByteCount == 0
					|| !SourceIdentities.insert(Source.StableIdentity).second)
				{
					OutError = "Import-record source identity, role, path, size, or uniqueness is invalid.";
					return false;
				}
			}
			std::unordered_set<std::string> OutputIdentities;
			std::unordered_set<FAssetPath> OutputPaths;
			for (const FImportRecordOutput& Output : State.Outputs)
			{
				if (!IsIdentifier(Output.StableIdentity))
				{
					OutError = "Import-record output identity is invalid.";
					return false;
				}
				if (Output.Role.empty() || Output.Role.size() > MaximumRecordStringBytes)
				{
					OutError = std::format(
						"Import-record output {} role is invalid.", Output.StableIdentity);
					return false;
				}
				if (!Output.AssetPath.IsValid()
					|| Output.AssetPathText != Output.AssetPath.ToString())
				{
					OutError = std::format(
						"Import-record output {} path is invalid.", Output.StableIdentity);
					return false;
				}
				if (Output.AssetClassName.empty()
					|| Output.AssetClassName.size() > MaximumRecordStringBytes)
				{
					OutError = std::format(
						"Import-record output {} class is invalid.", Output.StableIdentity);
					return false;
				}
				if (!OutputIdentities.insert(Output.StableIdentity).second
					|| !OutputPaths.insert(Output.AssetPath).second)
				{
					OutError = std::format(
						"Import-record output {} identity or path is duplicated.",
						Output.StableIdentity);
					return false;
				}
				if (Output.Policy == EImportRecordOutputPolicy::Managed
					&& Output.AuthoredFingerprint.empty())
				{
					OutError = "Managed import-record output has no authored fingerprint.";
					return false;
				}
			}
			std::unordered_set<std::string> TombstoneIdentities;
			uint64 PreviousSequence = 0;
			for (const FImportRecordDetachedTombstone& Tombstone : State.DetachedTombstones)
			{
				if (!IsIdentifier(Tombstone.StableIdentity) || !Tombstone.LastAssetPath.IsValid()
					|| Tombstone.LastAssetPathText != Tombstone.LastAssetPath.ToString()
					|| Tombstone.Sequence <= PreviousSequence
					|| !TombstoneIdentities.insert(Tombstone.StableIdentity).second)
				{
					OutError = "Import-record detached tombstones are invalid, duplicated, or unordered.";
					return false;
				}
				PreviousSequence = Tombstone.Sequence;
			}
			std::unordered_set<std::string> DiagnosticIdentities;
			for (const FImportRecordDiagnostic& Diagnostic : State.AcceptedDiagnostics)
			{
				if (Diagnostic.Severity
						> static_cast<uint8>(EImportDiagnosticSeverity::Error)
					|| Diagnostic.Category
						> static_cast<uint8>(EImportDiagnosticCategory::StalePlan)
					|| !IsIdentifier(Diagnostic.Identity)
					|| Diagnostic.Phase.empty()
					|| Diagnostic.SourceIdentity.empty()
					|| Diagnostic.OutputIdentity.empty()
					|| Diagnostic.Message.empty()
					|| Diagnostic.Phase.size() > MaximumRecordStringBytes
					|| Diagnostic.SourceIdentity.size() > MaximumRecordStringBytes
					|| Diagnostic.OutputIdentity.size() > MaximumRecordStringBytes
					|| Diagnostic.Message.size() > MaximumRecordStringBytes * 4
					|| !DiagnosticIdentities.insert(Diagnostic.Identity).second)
				{
					OutError = "Import-record accepted diagnostics are invalid or duplicated.";
					return false;
				}
			}
			if (State.PrimaryOutput.IsValid() && !OutputPaths.contains(State.PrimaryOutput))
			{
				OutError = "Import-record primary output is not one of the recorded outputs.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto SynchronizePathText(FImportRecordState& State) -> void
		{
			for (FImportRecordOutput& Output : State.Outputs)
				Output.AssetPathText = Output.AssetPath.ToString();
			for (FImportRecordDetachedTombstone& Tombstone : State.DetachedTombstones)
				Tombstone.LastAssetPathText = Tombstone.LastAssetPath.ToString();
		}

		auto UpdateString(FXxHash128Builder& Builder, std::string_view Value) -> void
		{
			const uint64 Size = Value.size();
			Builder.UpdateValue(Size);
			Builder.Update(Value);
		}

	}

	DImportRecord::DImportRecord(const FObjectInitializer& ObjectInitializer)
		: DObject(ObjectInitializer), RecordId(FGuid::NewGuid())
	{}

	auto DImportRecord::GetState() const -> FImportRecordState
	{
		return {
			.ProviderId = ProviderId,
			.ProviderContractVersion = ProviderContractVersion,
			.Settings = Settings,
			.ProviderState = ProviderState,
			.Sources = Sources,
			.Outputs = Outputs,
			.DetachedTombstones = DetachedTombstones,
			.AcceptedDiagnostics = AcceptedDiagnostics,
			.PrimaryOutput = PrimaryOutput};
	}

	auto DImportRecord::SetState(FImportRecordState State, std::string& OutError) -> bool
	{
		SynchronizePathText(State);
		if (!ValidateState(State, OutError)) return false;
		ProviderId = std::move(State.ProviderId);
		ProviderContractVersion = State.ProviderContractVersion;
		Settings = std::move(State.Settings);
		ProviderState = std::move(State.ProviderState);
		Sources = std::move(State.Sources);
		Outputs = std::move(State.Outputs);
		DetachedTombstones = std::move(State.DetachedTombstones);
		AcceptedDiagnostics = std::move(State.AcceptedDiagnostics);
		PrimaryOutput = std::move(State.PrimaryOutput);
		PrimaryOutputPathText = PrimaryOutput.ToString();
		RecordVersion = ImportRecordVersion;
		bExcludedFromCook = true;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DImportRecord::SetRecordIdForClone(const FGuid& NewId, std::string& OutError) -> bool
	{
		if (!NewId.IsValid() || NewId == RecordId)
		{
			OutError = "Import-record clone requires a distinct valid record identifier.";
			return false;
		}
		RecordId = NewId;
		for (FImportRecordOutput& Output : Outputs)
			if (Output.Policy == EImportRecordOutputPolicy::Managed)
				Output.Policy = EImportRecordOutputPolicy::Detached;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DImportRecord::ReplaceOutputPath(
		const FAssetPath& OldPath,
		const FAssetPath& NewPath) -> bool
	{
		if (!OldPath.IsValid() || !NewPath.IsValid()
			|| std::ranges::any_of(Outputs, [&](const FImportRecordOutput& Output) {
				return Output.AssetPath == NewPath;
			})) return false;
		const auto It = std::ranges::find(Outputs, OldPath, &FImportRecordOutput::AssetPath);
		if (It == Outputs.end()) return false;
		It->AssetPath = NewPath;
		It->AssetPathText = NewPath.ToString();
		if (PrimaryOutput == OldPath) PrimaryOutput = NewPath;
		PrimaryOutputPathText = PrimaryOutput.ToString();
		MarkPackageDirty();
		return true;
	}

	auto DImportRecord::Validate(std::string& OutError) const -> bool
	{
		if ((RecordVersion < MinimumSupportedImportRecordVersion
			|| RecordVersion > ImportRecordVersion)
			|| !RecordId.IsValid() || !bExcludedFromCook)
		{
			OutError = "Import-record header, identifier, or cook-exclusion marker is invalid.";
			return false;
		}
		return ValidateState(GetState(), OutError);
	}

	auto DImportRecord::GetFingerprint() const -> std::string
	{
		FXxHash128Builder Builder;
		Builder.UpdateValue(RecordVersion);
		Builder.UpdateValue(RecordId.A); Builder.UpdateValue(RecordId.B);
		Builder.UpdateValue(RecordId.C); Builder.UpdateValue(RecordId.D);
		UpdateString(Builder, ProviderId);
		Builder.UpdateValue(ProviderContractVersion);
		auto AddPayload = [&](const FImportRecordPayload& Payload) {
			UpdateString(Builder, Payload.SchemaId);
			Builder.UpdateValue(Payload.SchemaVersion);
			Builder.UpdateValue(Payload.ContentHashLow);
			Builder.UpdateValue(Payload.ContentHashHigh);
		};
		AddPayload(Settings);
		AddPayload(ProviderState);
		for (const FImportRecordSource& Source : Sources)
		{
			UpdateString(Builder, Source.StableIdentity); UpdateString(Builder, Source.Role);
			UpdateString(Builder, Source.SourcePath.Path);
			Builder.UpdateValue(Source.ContentHashLow); Builder.UpdateValue(Source.ContentHashHigh);
			Builder.UpdateValue(Source.ByteCount);
		}
		for (const FImportRecordOutput& Output : Outputs)
		{
			UpdateString(Builder, Output.StableIdentity); UpdateString(Builder, Output.Role);
			UpdateString(Builder, Output.AssetPath.GetView()); UpdateString(Builder, Output.AssetClassName);
			Builder.UpdateValue(Output.Policy); UpdateString(Builder, Output.AuthoredFingerprint);
		}
		for (const FImportRecordDetachedTombstone& Tombstone : DetachedTombstones)
		{
			UpdateString(Builder, Tombstone.StableIdentity);
			UpdateString(Builder, Tombstone.LastAssetPath.GetView());
			UpdateString(Builder, Tombstone.LastAuthoredFingerprint);
			Builder.UpdateValue(Tombstone.Sequence);
		}
		for (const FImportRecordDiagnostic& Diagnostic : AcceptedDiagnostics)
		{
			UpdateString(Builder, Diagnostic.Identity);
			Builder.UpdateValue(Diagnostic.Severity);
			Builder.UpdateValue(Diagnostic.Category);
			UpdateString(Builder, Diagnostic.Phase);
			UpdateString(Builder, Diagnostic.SourceIdentity);
			UpdateString(Builder, Diagnostic.OutputIdentity);
			UpdateString(Builder, Diagnostic.Message);
		}
		UpdateString(Builder, PrimaryOutput.GetView());
		return Builder.Finalize().ToString();
	}

	auto DImportRecord::ExchangeImportedState(DImportRecord& Other) noexcept -> void
	{
		if (&Other == this) return;
		std::swap(RecordVersion, Other.RecordVersion);
		std::swap(ProviderId, Other.ProviderId);
		std::swap(ProviderContractVersion, Other.ProviderContractVersion);
		std::swap(Settings, Other.Settings);
		std::swap(ProviderState, Other.ProviderState);
		std::swap(Sources, Other.Sources);
		std::swap(Outputs, Other.Outputs);
		std::swap(DetachedTombstones, Other.DetachedTombstones);
		std::swap(AcceptedDiagnostics, Other.AcceptedDiagnostics);
		std::swap(PrimaryOutput, Other.PrimaryOutput);
		std::swap(PrimaryOutputPathText, Other.PrimaryOutputPathText);
		std::swap(bExcludedFromCook, Other.bExcludedFromCook);
		MarkPackageDirty();
		Other.MarkPackageDirty();
	}

	auto DImportRecord::PostLoad(std::string& OutError) -> bool
	{
		if (!DObject::PostLoad(OutError)) return false;
		if (!PrimaryOutputPathText.empty()
			&& !FAssetPath::TryCreate(PrimaryOutputPathText, PrimaryOutput, &OutError))
			return false;
		return Validate(OutError);
	}

	auto MakeImportRecordPayload(
		std::string SchemaId,
		uint32 SchemaVersion,
		std::span<const uint8> Bytes,
		uint64 MaximumBytes,
		FImportRecordPayload& OutPayload,
		std::string& OutError) -> bool
	{
		if (!IsIdentifier(SchemaId) || SchemaVersion == 0 || Bytes.size() > MaximumBytes)
		{
			OutError = "Import-record payload schema or byte count is invalid.";
			return false;
		}
		const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
		OutPayload = {
			.SchemaId = std::move(SchemaId),
			.SchemaVersion = SchemaVersion,
			.Bytes = std::vector<uint8>(Bytes.begin(), Bytes.end()),
			.ContentHashLow = Hash.HashLow,
			.ContentHashHigh = Hash.HashHigh};
		OutError.clear();
		return true;
	}

	auto MakeSiblingImportRecordPath(
		const FAssetPath& SiblingOutput,
		std::string_view SourceName,
		FAssetPath& OutPath,
		std::string& OutError) -> bool
	{
		if (!SiblingOutput.IsValid() || SourceName.empty())
		{
			OutError = "Import-record sibling path requires an output and source name.";
			return false;
		}
		std::string Stem = std::filesystem::path(SourceName).stem().string();
		for (char& Character : Stem)
			if (!std::isalnum(static_cast<unsigned char>(Character)) && Character != '_') Character = '_';
		if (Stem.empty()) Stem = "Import";
		const std::filesystem::path Parent =
			std::filesystem::path(SiblingOutput.ToString()).parent_path();
		const std::string Candidate =
			(Parent / std::format("{}_Import", Stem)).generic_string();
		if (!FAssetPath::TryCreate(Candidate, OutPath, &OutError)) return false;
		OutError.clear();
		return true;
	}
}
