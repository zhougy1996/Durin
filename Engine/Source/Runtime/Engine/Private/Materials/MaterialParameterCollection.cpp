#include "MaterialParameterValidation.h"
#include "Materials/MaterialParameterCollection.h"

#include "Engine/World.h"
#include "DObject/Property.h"
#include "Rendering/SceneInterface.h"
#include "Threading/RunnableThread.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		auto IsNumericType(EMaterialParameterType Type) -> bool
		{
			return Type == EMaterialParameterType::Scalar
				|| Type == EMaterialParameterType::Vector2
				|| Type == EMaterialParameterType::Vector
				|| Type == EMaterialParameterType::Vector4;
		}

		auto ComponentCount(EMaterialParameterType Type) -> uint32
		{
			switch (Type)
			{
			case EMaterialParameterType::Scalar: return 1;
			case EMaterialParameterType::Vector2: return 2;
			case EMaterialParameterType::Vector: return 3;
			case EMaterialParameterType::Vector4: return 4;
			default: return 0;
			}
		}

		auto IsFinite(const FVector4& Value, EMaterialParameterType Type) -> bool
		{
			const std::array Components{Value.x, Value.y, Value.z, Value.w};
			return Private::IsFiniteMaterialComponents(
				std::span(Components).first(ComponentCount(Type)));
		}

		auto CanonicalValue(const FVector4& Value, EMaterialParameterType Type)
			-> FVector4
		{
			switch (Type)
			{
			case EMaterialParameterType::Scalar: return {Value.x, 0.0, 0.0, 0.0};
			case EMaterialParameterType::Vector2: return {Value.x, Value.y, 0.0, 0.0};
			case EMaterialParameterType::Vector: return {Value.x, Value.y, Value.z, 0.0};
			case EMaterialParameterType::Vector4: return Value;
			default: return {};
			}
		}

		auto FoldName(FName Name) -> std::string
		{
			auto Result = Name.ToString();
			std::ranges::transform(Result, Result.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Result;
		}

		auto Error(EMaterialParameterCollectionError Code, uint32 Index,
			FGuid Id, std::string Message) -> FMaterialParameterCollectionResult
		{
			return {.Error = Code, .RecordIndex = Index,
				.ParameterId = Id, .Message = std::move(Message)};
		}

		auto CanonicalDeclarations(const DMaterialParameterCollection& Collection)
			-> std::vector<FMaterialParameterCollectionDeclaration>
		{
			std::vector Result(Collection.GetDeclarations().begin(),
				Collection.GetDeclarations().end());
			std::ranges::sort(Result, {}, &FMaterialParameterCollectionDeclaration::Id);
			return Result;
		}
	}

	DMaterialParameterCollection::DMaterialParameterCollection(
		const FObjectInitializer& Initializer)
		: Super(Initializer)
		, CollectionId(IsTemplateConstructionPurpose(Initializer.Purpose)
			? FGuid{} : FGuid::NewGuid())
	{
	}

	auto DMaterialParameterCollection::PostLoad() -> void
	{
		Super::PostLoad();
		if (!CollectionId.IsValid()) CollectionId = FGuid::NewGuid();
		const auto Result = Validate();
		if (!Result)
			DURIN_ERROR("PostLoad '{}': invalid material parameter collection: {}",
				GetObjectPath(), Result.Message);
		else
		{
			for (auto& Declaration : Declarations)
				Declaration.DefaultValue = CanonicalValue(
					Declaration.DefaultValue, Declaration.Type);
			LastValidDeclarations = Declarations;
		}
	}

	auto DMaterialParameterCollection::PostEditChangeProperty(
		const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty
			|| Event.MemberProperty->NamePrivate != FName("Declarations")) return;
		if (auto Result = Validate(); !Result)
		{
			Declarations = LastValidDeclarations;
			DURIN_ERROR("Rejected material parameter collection edit '{}': {}",
				GetObjectPath(), Result.Message);
			return;
		}
		for (auto& Declaration : Declarations)
			Declaration.DefaultValue = CanonicalValue(
				Declaration.DefaultValue, Declaration.Type);
		if (Declarations == LastValidDeclarations) return;
		auto Previous = LastValidDeclarations;
		CommitDeclarationMutation(Previous);
	}

	auto DMaterialParameterCollection::Validate() const
		-> FMaterialParameterCollectionResult
	{
		if (!CollectionId.IsValid() || SchemaVersion != CurrentMaterialParameterCollectionSchemaVersion)
			return Error(EMaterialParameterCollectionError::InvalidCollection, 0, {},
				"The collection identity or schema version is invalid.");
		if (Declarations.size() > MaterialParameterCollectionMaxDeclarationCount)
			return Error(EMaterialParameterCollectionError::DeclarationLimit,
				static_cast<uint32>(Declarations.size()), {},
				"A collection supports at most 128 declarations.");
		std::unordered_set<FGuid> Ids;
		std::unordered_set<std::string> Names;
		for (uint32 Index = 0; Index < Declarations.size(); ++Index)
		{
			const auto& Declaration = Declarations[Index];
			if (!Declaration.Id.IsValid() || Declaration.Name.IsNone())
				return Error(EMaterialParameterCollectionError::InvalidDeclaration,
					Index, Declaration.Id, "A declaration requires a valid GUID and name.");
			if (!Ids.insert(Declaration.Id).second)
				return Error(EMaterialParameterCollectionError::DuplicateId,
					Index, Declaration.Id, "Declaration GUIDs must be unique.");
			if (!Names.insert(FoldName(Declaration.Name)).second)
				return Error(EMaterialParameterCollectionError::DuplicateName,
					Index, Declaration.Id, "Declaration names must be case-insensitively unique.");
			if (!IsNumericType(Declaration.Type))
				return Error(EMaterialParameterCollectionError::UnsupportedType,
					Index, Declaration.Id, "Collections support only numeric parameter types.");
			if (!IsFinite(Declaration.DefaultValue, Declaration.Type))
				return Error(EMaterialParameterCollectionError::NonFiniteValue,
					Index, Declaration.Id, "Collection defaults must be finite.");
		}
		return {};
	}

	auto DMaterialParameterCollection::BuildLayout() const
		-> std::expected<FMaterialParameterCollectionLayout,
			FMaterialParameterCollectionResult>
	{
		if (auto Validation = Validate(); !Validation)
			return std::unexpected(std::move(Validation));
		const auto Ordered = CanonicalDeclarations(*this);
		std::vector<FMaterialCompilerParameterDeclaration> Parameters;
		Parameters.reserve(Ordered.size());
		for (const auto& Declaration : Ordered)
			Parameters.push_back({Declaration.Id, Declaration.Type});
		auto Compiled = CompileMaterialLayout(Parameters);
		if (!Compiled)
			return std::unexpected(Error(
				EMaterialParameterCollectionError::InvalidDeclaration, 0,
				Compiled.Validation.ParameterId,
				"The collection uniform layout could not be compiled."));
		FMaterialParameterCollectionLayout Result{
			.CollectionId = CollectionId, .UniformLayout = std::move(Compiled.Layout),
			.SchemaVersion = SchemaVersion};
		FObjectPath ObjectPath;
		if (FObjectPath::TryCreate(GetObjectPath(), ObjectPath)
			&& ObjectPath.IsTopLevelAsset())
			Result.AssetPath = ObjectPath.ToString();
		Result.DefaultPayload.resize(Result.UniformLayout.UniformPayloadSize);
		for (const auto& Declaration : Ordered)
		{
			const auto Field = std::ranges::find(Result.UniformLayout.Fields,
				Declaration.Id, &FMaterialRenderField::ParameterId);
			check(Field != Result.UniformLayout.Fields.end());
			const std::array<float, 4> Components{
				static_cast<float>(Declaration.DefaultValue.x),
				static_cast<float>(Declaration.DefaultValue.y),
				static_cast<float>(Declaration.DefaultValue.z),
				static_cast<float>(Declaration.DefaultValue.w)};
			std::memcpy(Result.DefaultPayload.data() + Field->Offset,
				Components.data(), Field->Size);
		}
		return Result;
	}

	auto DMaterialParameterCollection::BuildDefaultPayload() const
		-> std::expected<FByteBuffer, FMaterialParameterCollectionResult>
	{
		auto Layout = BuildLayout();
		if (!Layout) return std::unexpected(Layout.error());
		return Layout->DefaultPayload;
	}

	auto DMaterialParameterCollection::SetDeclarations(
		std::span<const FMaterialParameterCollectionDeclaration> InDeclarations)
		-> FMaterialParameterCollectionResult
	{
		check(IsInGameThread());
		auto Previous = Declarations;
		Declarations.assign(InDeclarations.begin(), InDeclarations.end());
		if (auto Validation = Validate(); !Validation)
		{
			Declarations = std::move(Previous);
			return Validation;
		}
		for (auto& Declaration : Declarations)
			Declaration.DefaultValue = CanonicalValue(
				Declaration.DefaultValue, Declaration.Type);
		if (Declarations == Previous) return {};
		CommitDeclarationMutation(Previous);
		return {};
	}

	auto DMaterialParameterCollection::CommitDeclarationMutation(
		std::span<const FMaterialParameterCollectionDeclaration> Previous) -> void
	{
		const auto Schema = [](const auto& Values) {
			std::vector<std::pair<FGuid, EMaterialParameterType>> Result;
			Result.reserve(Values.size());
			for (const auto& Value : Values) Result.emplace_back(Value.Id, Value.Type);
			std::ranges::sort(Result);
			return Result;
		};
		const bool bSchemaChanged = Schema(Previous) != Schema(Declarations);
		if (bSchemaChanged) ++SchemaRevision;
		else
		{
			const auto Defaults = [](const auto& Values) {
				std::vector<std::pair<FGuid, FVector4>> Result;
				Result.reserve(Values.size());
				for (const auto& Value : Values)
					Result.emplace_back(Value.Id, Value.DefaultValue);
				std::ranges::sort(Result, {}, [](const auto& Value) {
					return Value.first;
				});
				return Result;
			};
			if (Defaults(Previous) != Defaults(Declarations)) ++DefaultsRevision;
		}
		MarkPackageDirty();
		NotifyMaterialParameterCollectionChanged(*this);
		LastValidDeclarations = Declarations;
	}

	auto DMaterialParameterCollection::SetDefaultValue(
		FGuid ParameterId, const FVector4& Value)
		-> FMaterialParameterCollectionResult
	{
		check(IsInGameThread());
		auto Declaration = std::ranges::find(Declarations, ParameterId,
			&FMaterialParameterCollectionDeclaration::Id);
		if (Declaration == Declarations.end())
			return Error(EMaterialParameterCollectionError::DeclarationNotFound,
				0, ParameterId, "The collection declaration was not found.");
		if (!IsFinite(Value, Declaration->Type))
			return Error(EMaterialParameterCollectionError::NonFiniteValue,
				0, ParameterId, "Collection defaults must be finite.");
		const auto Canonical = CanonicalValue(Value, Declaration->Type);
		if (Declaration->DefaultValue == Canonical) return {};
		Declaration->DefaultValue = Canonical;
		++DefaultsRevision;
		MarkPackageDirty();
		NotifyMaterialParameterCollectionChanged(*this);
		LastValidDeclarations = Declarations;
		return {};
	}

	auto DMaterialParameterCollection::FindDeclaration(FGuid ParameterId) const
		-> const FMaterialParameterCollectionDeclaration*
	{
		const auto Found = std::ranges::find(Declarations, ParameterId,
			&FMaterialParameterCollectionDeclaration::Id);
		return Found == Declarations.end() ? nullptr : &*Found;
	}

	auto DMaterialParameterCollection::FindDeclaration(FName Name) const
		-> const FMaterialParameterCollectionDeclaration*
	{
		const auto Folded = FoldName(Name);
		const auto Found = std::ranges::find_if(Declarations,
			[&](const auto& Declaration) {
				return FoldName(Declaration.Name) == Folded;
			});
		return Found == Declarations.end() ? nullptr : &*Found;
	}

	DMaterialParameterCollectionSubsystem::DMaterialParameterCollectionSubsystem(
		const FObjectInitializer& Initializer) : Super(Initializer)
	{
	}

	auto DMaterialParameterCollectionSubsystem::FindOrCreateState(
		DMaterialParameterCollection& Collection)
		-> std::expected<FState*, FMaterialParameterCollectionResult>
	{
		check(IsInGameThread());
		auto Layout = Collection.BuildLayout();
		if (!Layout) return std::unexpected(Layout.error());
		if (auto Existing = States.find(Collection.GetCollectionId());
			Existing != States.end())
		{
			if (Existing->second.Collection.Get() != &Collection)
				return std::unexpected(Error(
					EMaterialParameterCollectionError::InvalidCollection, 0, {},
					"Two live collections use the same persistent identity."));
			if (!Existing->second.Layout.HasCompatibleSchema(*Layout))
			{
				Existing->second.Layout = std::move(*Layout);
				for (auto Override = Existing->second.Overrides.begin();
					Override != Existing->second.Overrides.end();)
					Override = Collection.FindDeclaration(Override->first)
						? std::next(Override) : Existing->second.Overrides.erase(Override);
			}
			else Existing->second.Layout.DefaultPayload = std::move(Layout->DefaultPayload);
			return &Existing->second;
		}
		FState State{.Collection = &Collection, .Layout = std::move(*Layout)};
		auto [Inserted, bInserted] = States.emplace(Collection.GetCollectionId(),
			std::move(State));
		check(bInserted);
		return &Inserted->second;
	}

	auto DMaterialParameterCollectionSubsystem::BuildSnapshot(const FState& State,
		const std::unordered_map<FGuid, FVector4>& Overrides, uint64 Version) const
		-> std::expected<std::shared_ptr<const FMaterialParameterCollectionSnapshot>,
			FMaterialParameterCollectionResult>
	{
		auto Payload = State.Layout.DefaultPayload;
		for (const auto& [Id, Value] : Overrides)
		{
			const auto Field = std::ranges::find(State.Layout.UniformLayout.Fields,
				Id, &FMaterialRenderField::ParameterId);
			if (Field == State.Layout.UniformLayout.Fields.end())
				return std::unexpected(Error(EMaterialParameterCollectionError::InvalidState, 0,
					Id, "A runtime override no longer matches the collection schema."));
			const std::array<float, 4> Components{
				static_cast<float>(Value.x), static_cast<float>(Value.y),
				static_cast<float>(Value.z), static_cast<float>(Value.w)};
			std::memcpy(Payload.data() + Field->Offset, Components.data(), Field->Size);
		}
		auto Snapshot = std::make_shared<FMaterialParameterCollectionSnapshot>();
		Snapshot->Layout = State.Layout;
		Snapshot->Version = Version;
		Snapshot->Payload = std::move(Payload);
		return Snapshot;
	}

	auto DMaterialParameterCollectionSubsystem::Publish(FState& State,
		std::unordered_map<FGuid, FVector4> Overrides)
		-> FMaterialParameterCollectionResult
	{
		auto Snapshot = BuildSnapshot(State, Overrides, State.Version + 1);
		if (!Snapshot) return Snapshot.error();
		State.Overrides = std::move(Overrides);
		State.Version = (*Snapshot)->Version;
		State.Snapshot = std::move(*Snapshot);
		++Counters.Publications;
		Counters.PayloadBytes += State.Snapshot->Payload.size();
		if (auto* World = GetWorld(); World && World->GetRenderScene())
			World->GetRenderScene()->UpdateMaterialParameterCollection(State.Snapshot);
		return {};
	}

	auto DMaterialParameterCollectionSubsystem::ApplyUpdates(
		DMaterialParameterCollection& Collection,
		std::span<const FMaterialParameterCollectionUpdate> Updates)
		-> FMaterialParameterCollectionResult
	{
		check(IsInGameThread());
		Counters.SubmittedRecords += Updates.size();
		if (Updates.empty()) { ++Counters.NoOpCommits; return {}; }
		auto StateResult = FindOrCreateState(Collection);
		if (!StateResult)
		{
			++Counters.RejectedCommits;
			return StateResult.error();
		}
		auto* State = *StateResult;
		std::unordered_set<FGuid> Seen;
		auto Candidate = State->Overrides;
		for (uint32 Index = 0; Index < Updates.size(); ++Index)
		{
			const auto& Update = Updates[Index];
			if (!Seen.insert(Update.ParameterId).second)
			{
				++Counters.RejectedCommits;
				return Error(EMaterialParameterCollectionError::DuplicateUpdate,
					Index, Update.ParameterId, "A batch cannot update one declaration twice.");
			}
			const auto* Declaration = Collection.FindDeclaration(Update.ParameterId);
			if (!Declaration)
			{
				++Counters.RejectedCommits;
				return Error(EMaterialParameterCollectionError::DeclarationNotFound,
					Index, Update.ParameterId, "The collection declaration was not found.");
			}
			if (Update.Operation == EMaterialParameterCollectionUpdateOperation::Clear)
				Candidate.erase(Update.ParameterId);
			else if (!IsFinite(Update.Value, Declaration->Type))
			{
				++Counters.RejectedCommits;
				return Error(EMaterialParameterCollectionError::NonFiniteValue,
					Index, Update.ParameterId, "Runtime collection values must be finite.");
			}
			else Candidate[Update.ParameterId] = CanonicalValue(Update.Value,
				Declaration->Type);
		}
		if (Candidate == State->Overrides)
		{
			++Counters.NoOpCommits;
			return {};
		}
		if (auto Published = Publish(*State, std::move(Candidate)); !Published)
		{
			++Counters.RejectedCommits;
			return Published;
		}
		++Counters.ChangedCommits;
		return {};
	}

	auto DMaterialParameterCollectionSubsystem::SetValue(
		DMaterialParameterCollection& Collection, FGuid ParameterId,
		const FVector4& Value) -> FMaterialParameterCollectionResult
	{
		const auto Update = FMaterialParameterCollectionUpdate::Set(ParameterId, Value);
		return ApplyUpdates(Collection, std::span(&Update, 1));
	}

	auto DMaterialParameterCollectionSubsystem::SetValue(
		DMaterialParameterCollection& Collection, FName ParameterName,
		const FVector4& Value) -> FMaterialParameterCollectionResult
	{
		const auto* Declaration = Collection.FindDeclaration(ParameterName);
		if (!Declaration)
		{
			++Counters.SubmittedRecords;
			++Counters.RejectedCommits;
			return Error(EMaterialParameterCollectionError::DeclarationNotFound,
				0, {}, "The collection declaration was not found.");
		}
		return SetValue(Collection, Declaration->Id, Value);
	}

	auto DMaterialParameterCollectionSubsystem::ClearValue(
		DMaterialParameterCollection& Collection, FGuid ParameterId)
		-> FMaterialParameterCollectionResult
	{
		const auto Update = FMaterialParameterCollectionUpdate::Clear(ParameterId);
		return ApplyUpdates(Collection, std::span(&Update, 1));
	}

	auto DMaterialParameterCollectionSubsystem::ClearValue(
		DMaterialParameterCollection& Collection, FName ParameterName)
		-> FMaterialParameterCollectionResult
	{
		const auto* Declaration = Collection.FindDeclaration(ParameterName);
		if (!Declaration)
		{
			++Counters.SubmittedRecords;
			++Counters.RejectedCommits;
			return Error(EMaterialParameterCollectionError::DeclarationNotFound,
				0, {}, "The collection declaration was not found.");
		}
		return ClearValue(Collection, Declaration->Id);
	}

	auto DMaterialParameterCollectionSubsystem::GetValue(
		const DMaterialParameterCollection& Collection, FGuid ParameterId,
		FVector4& OutValue) const -> bool
	{
		check(IsInGameThread());
		const auto State = States.find(Collection.GetCollectionId());
		if (State != States.end())
			if (const auto Override = State->second.Overrides.find(ParameterId);
				Override != State->second.Overrides.end())
			{
				OutValue = Override->second;
				return true;
			}
		const auto* Declaration = Collection.FindDeclaration(ParameterId);
		if (!Declaration) return false;
		OutValue = Declaration->DefaultValue;
		return true;
	}

	auto DMaterialParameterCollectionSubsystem::GetValue(
		const DMaterialParameterCollection& Collection, FName ParameterName,
		FVector4& OutValue) const -> bool
	{
		const auto* Declaration = Collection.FindDeclaration(ParameterName);
		return Declaration && GetValue(Collection, Declaration->Id, OutValue);
	}

	auto DMaterialParameterCollectionSubsystem::GetSnapshot(
		const DMaterialParameterCollection& Collection) const
		-> std::shared_ptr<const FMaterialParameterCollectionSnapshot>
	{
		check(IsInGameThread());
		const auto State = States.find(Collection.GetCollectionId());
		return State == States.end() ? nullptr : State->second.Snapshot;
	}

	auto DMaterialParameterCollectionSubsystem::RefreshCollection(
		DMaterialParameterCollection& Collection)
		-> FMaterialParameterCollectionResult
	{
		check(IsInGameThread());
		if (!States.contains(Collection.GetCollectionId())) return {};
		auto StateResult = FindOrCreateState(Collection);
		if (!StateResult) return StateResult.error();
		auto* State = *StateResult;
		auto Candidate = BuildSnapshot(*State, State->Overrides, State->Version + 1);
		if (!Candidate) return Candidate.error();
		if (State->Snapshot
			&& State->Snapshot->Layout.HasCompatibleSchema((*Candidate)->Layout)
			&& State->Snapshot->Payload == (*Candidate)->Payload) return {};
		if (auto Published = Publish(*State, State->Overrides); !Published)
			return Published;
		++Counters.AssetRefreshPublications;
		return {};
	}

	auto DMaterialParameterCollectionSubsystem::AddReferencedObjects(
		FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		for (auto& [Id, State] : States)
		{
			DObject* Collection = State.Collection.Get();
			Collector.AddReferencedObject(Collection);
		}
	}

	auto DMaterialParameterCollectionSubsystem::Deinitialize() noexcept -> void
	{
		if (auto* World = GetWorld(); World && World->GetRenderScene())
			for (const auto& [Id, State] : States)
				World->GetRenderScene()->RemoveMaterialParameterCollection(Id);
		States.clear();
	}

	auto DMaterialParameterCollectionSubsystem::OnRenderSceneChanged(
		FSceneInterface* Previous, FSceneInterface* Current) noexcept -> void
	{
		for (const auto& [Id, State] : States)
		{
			if (Previous) Previous->RemoveMaterialParameterCollection(Id);
			if (Current && State.Snapshot)
				Current->UpdateMaterialParameterCollection(State.Snapshot);
		}
	}
}
