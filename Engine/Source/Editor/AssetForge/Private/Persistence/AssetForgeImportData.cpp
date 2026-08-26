#include "AssetForge/Persistence/AssetForgeImportData.h"

#include "DObject/AssetPath.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::AssetForge
{
	namespace
	{
		auto IsIdentifier(std::string_view Value) -> bool
		{
			return !Value.empty() && Value.size() <= AssetImport::MaximumAssetImportStringBytes
				&& std::ranges::all_of(Value, [](unsigned char Character) {
					return std::isalnum(Character) || Character == '.' || Character == '_'
						|| Character == '-' || Character == ':' || Character == '/'
						|| Character == '+';
				});
		}

		auto IsEmptyReplayState(const FAssetForgeImportState& State) -> bool
		{
			return State.SourceData.Sources.empty()
				&& State.Translator.ComponentId.empty()
				&& State.Translator.ContractVersion == 0
				&& State.Translator.Settings.IsEmpty()
				&& State.PlanningPassStack.empty()
				&& State.SourceReferences.empty()
				&& State.OutputMappings.empty()
				&& State.SourceGraphFingerprintLow == 0
				&& State.SourceGraphFingerprintHigh == 0
				&& State.BuildGraphFingerprintLow == 0
				&& State.BuildGraphFingerprintHigh == 0
				&& State.AuthoredOutputFingerprint.empty();
		}

		auto ValidateAssetForgeState(
			const FAssetForgeImportState& State, std::string& OutError) -> bool
		{
			if (State.SchemaVersion != AssetImport::AssetImportDataSchemaVersion
				|| !State.SourceData.Validate(OutError))
			{
				if (OutError.empty()) OutError = "Unsupported asset-import-data schema version.";
				return false;
			}
			if (State.ReplaySchemaVersion != AssetForgeImportDataSchemaVersion)
			{
				OutError = std::format(
					"Unsupported interchange replay schema version {}.",
					State.ReplaySchemaVersion);
				return false;
			}
			if (IsEmptyReplayState(State))
			{
				OutError.clear();
				return true;
			}
			if (State.SourceData.Sources.empty()
				|| State.PlanningPassStack.size() > MaximumAssetImportPlanningPasses
				|| State.SourceReferences.empty()
				|| State.SourceReferences.size() > AssetImport::MaximumAssetImportSources
				|| State.OutputMappings.empty()
				|| State.OutputMappings.size() > MaximumAssetImportOutputMappings
				|| State.SourceGraphFingerprintLow == 0
				|| State.SourceGraphFingerprintHigh == 0
				|| State.BuildGraphFingerprintLow == 0
				|| State.BuildGraphFingerprintHigh == 0
				|| State.AuthoredOutputFingerprint.empty()
				|| State.AuthoredOutputFingerprint.size()
					> AssetImport::MaximumAssetImportStringBytes)
			{
				OutError = "AssetForge import replay state is incomplete or exceeds its bounds.";
				return false;
			}
			if (!State.Translator.Validate(OutError)) return false;
			uint64 PayloadBytes = State.Translator.Settings.Bytes.size();
			for (const FAssetImportPlanningPassDescriptor& Pass : State.PlanningPassStack)
			{
				if (!Pass.Validate(OutError)) return false;
				if (Pass.Settings.Bytes.size() > MaximumAssetImportPayloadBytes - PayloadBytes)
				{
					OutError = "Aggregate import settings exceed the replay-state byte limit.";
					return false;
				}
				PayloadBytes += Pass.Settings.Bytes.size();
			}
			std::unordered_set<std::string> SourceIdentities;
			for (const FAssetImportSourceReference& Reference : State.SourceReferences)
			{
				if (!Reference.Validate(OutError)
					|| !SourceIdentities.insert(Reference.StableIdentity).second
					|| !State.SourceData.FindByStableIdentity(Reference.StableIdentity))
				{
					if (OutError.empty())
						OutError = "Replay source references must be unique and resolve by stable identity.";
					return false;
				}
			}
			if (SourceIdentities.size() != State.SourceData.Sources.size())
			{
				OutError = "Replay source references must cover every persisted source exactly once.";
				return false;
			}
			std::unordered_set<std::string> OutputIdentities;
			for (const FAssetImportOutputMapping& Mapping : State.OutputMappings)
			{
				if (!Mapping.Validate(OutError)
					|| !OutputIdentities.insert(Mapping.OutputIdentity).second)
				{
					if (OutError.empty()) OutError = "Output-mapping identities must be unique.";
					return false;
				}
			}
			OutError.clear();
			return true;
		}
	}

	auto FAssetImportPayload::IsEmpty() const -> bool
	{
		return SchemaId.empty() && SchemaVersion == 0 && Bytes.empty()
			&& ContentHashLow == 0 && ContentHashHigh == 0;
	}

	auto FAssetImportPayload::Validate(
		uint64 MaximumBytes, std::string& OutError) const -> bool
	{
		if (IsEmpty())
		{
			OutError.clear();
			return true;
		}
		if (!IsIdentifier(SchemaId) || SchemaVersion == 0 || Bytes.size() > MaximumBytes)
		{
			OutError = "Import payload identity, version, or byte count is invalid.";
			return false;
		}
		const FXxHash128 Hash = FXxHash128::HashBuffer(std::span<const std::byte>(Bytes));
		if (Hash.HashLow != ContentHashLow || Hash.HashHigh != ContentHashHigh)
		{
			OutError = "Import payload hash does not match its bytes.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto FAssetImportComponentDescriptor::Validate(std::string& OutError) const -> bool
	{
		if (!IsIdentifier(ComponentId) || ContractVersion == 0)
		{
			OutError = "Import component identity or contract version is invalid.";
			return false;
		}
		return Settings.Validate(MaximumAssetImportSettingsBytes, OutError);
	}

	auto FAssetImportPlanningPassDescriptor::Validate(std::string& OutError) const -> bool
	{
		if (!IsIdentifier(PlanningPassId) || ContractVersion == 0)
		{
			OutError = "Planning-pass identity or contract version is invalid.";
			return false;
		}
		return Settings.Validate(MaximumAssetImportSettingsBytes, OutError);
	}

	auto FAssetImportSourceReference::Validate(std::string& OutError) const -> bool
	{
		if (!IsIdentifier(StableIdentity))
		{
			OutError = "Replay source reference identity is invalid.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto FAssetImportOutputMapping::Validate(std::string& OutError) const -> bool
	{
		FAssetPath Path;
		if (!IsIdentifier(SourceNodeIdentity) || !IsIdentifier(OutputIdentity)
			|| !FAssetPath::TryCreate(AssetPathText, Path, &OutError))
		{
			if (OutError.empty())
				OutError = "Import output mapping identity or asset path is invalid.";
			return false;
		}
		OutError.clear();
		return true;
	}

	DAssetForgeImportData::DAssetForgeImportData(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DAssetForgeImportData::SetState(
		FAssetForgeImportState State, std::string& OutError) -> bool
	{
		State.SourceData.Normalize();
		if (!ValidateAssetForgeState(State, OutError)) return false;
		AssetImport::FAssetImportDataState BaseState = State;
		if (!ApplyState(std::move(BaseState), OutError)) return false;
		ReplaySchemaVersion = State.ReplaySchemaVersion;
		Translator = std::move(State.Translator);
		PlanningPassStack = std::move(State.PlanningPassStack);
		SourceReferences = std::move(State.SourceReferences);
		OutputMappings = std::move(State.OutputMappings);
		SourceGraphFingerprintLow = State.SourceGraphFingerprintLow;
		SourceGraphFingerprintHigh = State.SourceGraphFingerprintHigh;
		BuildGraphFingerprintLow = State.BuildGraphFingerprintLow;
		BuildGraphFingerprintHigh = State.BuildGraphFingerprintHigh;
		AuthoredOutputFingerprint = std::move(State.AuthoredOutputFingerprint);
		OutError.clear();
		return true;
	}

	auto DAssetForgeImportData::GetAssetForgeState() const
		-> FAssetForgeImportState
	{
		FAssetForgeImportState State;
		static_cast<AssetImport::FAssetImportDataState&>(State) = {
			.SchemaVersion = GetSchemaVersion(), .SourceData = GetSourceData()};
		State.ReplaySchemaVersion = ReplaySchemaVersion;
		State.Translator = Translator;
		State.PlanningPassStack = PlanningPassStack;
		State.SourceReferences = SourceReferences;
		State.OutputMappings = OutputMappings;
		State.SourceGraphFingerprintLow = SourceGraphFingerprintLow;
		State.SourceGraphFingerprintHigh = SourceGraphFingerprintHigh;
		State.BuildGraphFingerprintLow = BuildGraphFingerprintLow;
		State.BuildGraphFingerprintHigh = BuildGraphFingerprintHigh;
		State.AuthoredOutputFingerprint = AuthoredOutputFingerprint;
		return State;
	}

	auto DAssetForgeImportData::Validate(std::string& OutError) const -> bool
	{
		return ValidateAssetForgeState(GetAssetForgeState(), OutError);
	}

	auto DAssetForgeImportData::CloneToOwner(
		DObject* Owner, FName Name, std::string& OutError) const
		-> AssetImport::DAssetImportData*
	{
		if (!Owner || Name.IsNone() || !Validate(OutError))
		{
			if (OutError.empty()) OutError = "Import-data clone requires a valid owner and name.";
			return nullptr;
		}
		auto* Clone = NewObject<DAssetForgeImportData>(
			Owner, Name, EObjectConstructionPurpose::Duplication);
		if (!Clone || !Clone->SetState(GetAssetForgeState(), OutError)) return nullptr;
		return Clone;
	}

	auto MakeAssetImportPayload(
		std::string SchemaId,
		uint32 SchemaVersion,
		std::span<const std::byte> Bytes,
		uint64 MaximumBytes,
		FAssetImportPayload& OutPayload,
		std::string& OutError) -> bool
	{
		if (Bytes.size() > MaximumBytes)
		{
			OutError = "Import payload exceeds its byte limit.";
			return false;
		}
		FAssetImportPayload Payload;
		Payload.SchemaId = std::move(SchemaId);
		Payload.SchemaVersion = SchemaVersion;
		Payload.Bytes.assign(Bytes.begin(), Bytes.end());
		const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
		Payload.ContentHashLow = Hash.HashLow;
		Payload.ContentHashHigh = Hash.HashHigh;
		if (!Payload.Validate(MaximumBytes, OutError)) return false;
		OutPayload = std::move(Payload);
		OutError.clear();
		return true;
	}

	auto ValidateAssetImportDataState(
		const FAssetForgeImportState& State,
		std::string& OutError) -> bool
	{
		return ValidateAssetForgeState(State, OutError);
	}
}
