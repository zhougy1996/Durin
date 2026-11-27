#include "MaterialParameterValidation.h"
#include "Materials/MaterialShaderMap.h"
#include "MaterialParameterMutation.h"
#include "MaterialDependencyIndex.h"
#include "Components/PropertyEditValidation.h"
#include "Materials/ObjectCacheContext.h"
#include "Materials/MaterialInstance.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionSwitch.h"
#include "Materials/MaterialObjectValidation.h"
#include "Materials/MaterialCustomVersion.h"
#include "Logging/LogMacros.h"
#include "Threading/RunnableThread.h"

#include "Asset/Asset.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Archive.h"

namespace Durin
{
	namespace
	{
		struct FMaterialDynamicParameterAtomicCounters
		{
			std::atomic<uint64> LogicalMutationCount = 0;
			std::atomic<uint64> ChangedCommitCount = 0;
			std::atomic<uint64> NoOpCommitCount = 0;
			std::atomic<uint64> RejectedCommitCount = 0;
			std::atomic<uint64> OwnerPublicationCount = 0;
		};

		FMaterialDynamicParameterAtomicCounters GMaterialDynamicParameterCounters;

		auto FindStaticBool(const DMaterialInstance& Instance, const FMaterialParameterInfo& Info, FGuid Id)
			-> const DMaterialExpressionStaticBool*
		{
#if DURIN_WITH_EDITORONLY_DATA
			const DMaterialInterface* Root = Instance.GetParent();
			for (uint32 Depth = 0; Root && Root->GetParent() && Depth < MaterialMaximumParentDepth; ++Depth)
				Root = Root->GetParent();
			if (const auto* Material = Cast<DMaterial>(Root))
				for (const auto& Node : Material->GetExpressionCollection().Expressions)
					if (const auto* Bool = Cast<DMaterialExpressionStaticBool>(Node.Get());
						Bool && (Id.IsValid() ? Bool->DeclarationId == Id : Info.IsGlobal() && Info.Name == Bool->Name))
						return Bool;
#endif
			return nullptr;
		}

		auto WouldCreateParentCycle(
			const DMaterialInstance* Instance,
			const DMaterialInterface* CandidateParent
		) -> bool
		{
			std::unordered_set<const DMaterialInterface*> Visited;
			for (const DMaterialInterface* Candidate = CandidateParent;
				Candidate != nullptr;
				Candidate = Candidate->GetParent())
			{
				if (Candidate == Instance || !Visited.insert(Candidate).second
					|| Visited.size() >= MaterialMaximumParentDepth) return true;
			}
			return false;
		}
	}

	auto GetMaterialDynamicParameterCounters()
		-> FMaterialDynamicParameterCounters
	{
		return {
			.LogicalMutationCount = GMaterialDynamicParameterCounters.LogicalMutationCount.load(),
			.ChangedCommitCount = GMaterialDynamicParameterCounters.ChangedCommitCount.load(),
			.NoOpCommitCount = GMaterialDynamicParameterCounters.NoOpCommitCount.load(),
			.RejectedCommitCount = GMaterialDynamicParameterCounters.RejectedCommitCount.load(),
			.OwnerPublicationCount = GMaterialDynamicParameterCounters.OwnerPublicationCount.load(),
		};
	}

	auto ResetMaterialDynamicParameterCounters() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		GMaterialDynamicParameterCounters.LogicalMutationCount.store(0);
		GMaterialDynamicParameterCounters.ChangedCommitCount.store(0);
		GMaterialDynamicParameterCounters.NoOpCommitCount.store(0);
		GMaterialDynamicParameterCounters.RejectedCommitCount.store(0);
		GMaterialDynamicParameterCounters.OwnerPublicationCount.store(0);
	}

	DMaterialInstance::DMaterialInstance(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose)) PublishMaterialRenderProxyState();
	}

	auto DMaterialInstance::CreateDynamic(DMaterialInterface* InParent, DObject* Outer, FName Name)
		-> DMaterialInstance*
	{
		if (!IsValid(InParent) || InParent->IsDynamicInstance()) return nullptr;
		FResolvedMaterialProperties Resolved;
		if (!ResolveMaterialProperties(*InParent, Resolved)) return nullptr;
		uint32 Depth = 1;
		for (auto* Node = InParent; Node; Node = Node->GetParent())
			if (++Depth > MaterialMaximumParentDepth) return nullptr;
		auto* Instance = NewObject<DMaterialInstance>(Outer, Name, EObjectFlags::Transient);
		Instance->bDynamicInstance = true;
		Instance->Parent = InParent;
		Private::RefreshMaterialDependency(*Instance);
		Instance->MarkRenderDataDirty(EMaterialRenderDirtyFlags::AllRenderState);
		return Instance;
	}

	auto DMaterialInstance::ValidateParameterStorage(const FPropertyEditProposal* Proposal) const -> std::expected<void, std::string>
	{
		std::unordered_set<FGuid> Ids;
		std::expected<void, std::string> Result;
		VisitParameterValueArrays([&](const auto& Stored) {
			if (!Result) return;
			using TArray = std::decay_t<decltype(Stored)>;
			using TRecord = typename TArray::value_type;
			std::unordered_set<FName> Names;
			const TArray* Records = &Stored;
			if (Proposal && Proposal->MemberProperty && Proposal->MemberProperty->NamePrivate == TRecord::PropertyName())
			{
				if (Proposal->DraftRootProperty != Proposal->MemberProperty || !Proposal->DraftRootContainer)
				{
					Result = std::unexpected("Material parameter edits require complete detached storage.");
					return;
				}
				Records = Proposal->DraftRootProperty->ContainerPtrToValuePtr<TArray>(
					Proposal->DraftRootContainer, Proposal->DraftRootArrayIndex);
			}
			for (uint32 Index = 0; Index < Records->size(); ++Index)
			{
				const auto& Record = (*Records)[Index];
				if (!Record.ParameterId.IsValid() && !Record.ParameterInfo.IsGlobal()) Result = std::unexpected("Material parameter value has an invalid identity.");
				else if (Record.ParameterId.IsValid() && !Ids.insert(Record.ParameterId).second) Result = std::unexpected("Material parameter value identity is duplicated.");
				else if (Record.ParameterInfo != FMaterialParameterInfo{} && (!Record.ParameterInfo.IsGlobal() || !Names.insert(Record.ParameterInfo.Name).second))
					Result = std::unexpected("Material parameter info is invalid or duplicated.");
				else if (!Private::IsFiniteMaterialParameterValue(Record.GetValue())) Result = std::unexpected("Material declaration default must be finite.");
				else if constexpr (std::is_same_v<TRecord, FMaterialTextureParameterValue>)
					if (!Record.Value.IsValid())
						Result = std::unexpected("Material parameter value has an invalid sampling policy.");
				if (!Result)
				{
					Result.error() += std::format(" [parameter {}] [index {}]", Record.ParameterId.ToString(), Index);
					return;
				}
			}
		});
		const std::vector<FMaterialStaticBoolOverride>* StaticRecords = &StaticBoolOverrides;
		if (Proposal && Proposal->MemberProperty
			&& Proposal->MemberProperty->NamePrivate == FName("StaticBoolOverrides"))
		{
			if (Proposal->DraftRootProperty != Proposal->MemberProperty
				|| !Proposal->DraftRootContainer)
				return std::unexpected("Material parameter edits require complete detached storage.");
			StaticRecords = Proposal->DraftRootProperty->ContainerPtrToValuePtr<
				std::vector<FMaterialStaticBoolOverride>>(
					Proposal->DraftRootContainer, Proposal->DraftRootArrayIndex);
		}
		std::unordered_set<FGuid> StaticIds;
		std::unordered_set<FName> StaticNames;
		if (StaticRecords->size() > MaterialMaxStaticBoolDeclarations)
			Result = std::unexpected("Material parameter value identity is duplicated.");
		for (uint32 Index = 0; Result && Index < StaticRecords->size(); ++Index)
		{
			const auto& Override = (*StaticRecords)[Index];
			if (!Override.DeclarationId.IsValid() && !Override.ParameterInfo.IsGlobal()) Result = std::unexpected("Material parameter value has an invalid identity.");
			else if ((Override.DeclarationId.IsValid() && !StaticIds.insert(Override.DeclarationId).second)
				|| (Override.ParameterInfo != FMaterialParameterInfo{} && (!Override.ParameterInfo.IsGlobal() || !StaticNames.insert(Override.ParameterInfo.Name).second)))
				Result = std::unexpected("Material parameter value identity is duplicated.");
			if (!Result)
			{
				Result.error() += std::format(" [parameter {}] [index {}]", Override.DeclarationId.ToString(), Index);
			}
		}
		return Result;
	}

	auto DMaterialInstance::Serialize(FArchive& Ar) -> void
	{
		if (bDynamicInstance)
		{
			// Package planning needs the normal field manifest before excluding
			// transient objects from the persisted graph. Other copies are forbidden.
			if (!(Ar.IsSaving() && (Ar.GetPurpose() == EArchivePurpose::Discovery
				|| Ar.GetPurpose() == EArchivePurpose::AuthoredPackage
				|| Ar.GetPurpose() == EArchivePurpose::CookedPackage)))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, "Dynamic material instances cannot be serialized or duplicated.");
				return;
			}
		}
		if (!FMaterialInstanceVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
		if (Ar.IsError()) return;
		if (Ar.IsLoading())
		{
			// A GUID-only orphan has no runtime address after editor data is stripped.
			if (Ar.IsFilterEditorOnly())
				VisitParameterValueArrays([](auto& Records) {
					std::erase_if(Records, [](const auto& Record) { return Record.ParameterInfo == FMaterialParameterInfo{}; });
				});
			Private::RefreshMaterialDependency(*this);
		}
		const auto Validation = ValidateParameterStorage();
		if (!Validation)
		{
			auto Rejection = RejectMaterialObjectGraph(GetObjectPath(), {{.Message = Validation.error()}});
			if (auto* ObjectArchive = dynamic_cast<FObjectArchive*>(&Ar))
				ObjectArchive->FailValidation(std::move(Rejection.error()));
			else Ar.Fail(EArchiveFailureCode::InvalidData, Validation.error());
		}
	}

	auto DMaterialInstance::SetParent(DMaterialInterface* InParent) -> bool
	{
		return SetParentAndPropertyOverrides(InParent, PropertyOverrides);
	}

	auto DMaterialInstance::SetParentAndPropertyOverrides(DMaterialInterface* InParent,
		const FMaterialPropertyOverrides& Overrides) -> bool
	{
		if (bDynamicInstance || (InParent && InParent->IsDynamicInstance())) return false;
		if (WouldCreateParentCycle(this, InParent)
			|| !ValidateMaterialStaticProperties(Overrides.Values)) return false;
		const bool bParentChanged = Parent != InParent;
		if (!bParentChanged && PropertyOverrides == Overrides) return true;
		Parent = InParent;
		if (bParentChanged) RefreshParameterBindings(true);
		Private::RefreshMaterialDependency(*this);
		PropertyOverrides = Overrides;
		FObjectCacheContext Context;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) AdoptParentRuntimeProgram();
		else InvalidateMaterialCompilation(true, !bParentChanged, &Context);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::AllRenderState, bParentChanged, &Context);
		return true;
	}

	auto DMaterialInstance::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
	{
		if (bDynamicInstance) return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::ModuleRejected);
		if (auto Result = Super::PreEditChangeProperty(Proposal); !Result) return Result;
		if (const auto Validation = ValidateParameterStorage(&Proposal); !Validation)
		{
			return RejectEnginePropertyEdit(*this, Proposal, Validation.error());
		}
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("PropertyOverrides")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty && Proposal.DraftRootContainer)
		{
			const auto* Overrides = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FMaterialPropertyOverrides>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			const auto Validation = ValidateMaterialStaticProperties(Overrides->Values);
			if (!Validation) return RejectEnginePropertyEdit(*this, Proposal, Validation.error());
			return {};
		}
		if (!Proposal.MemberProperty || Proposal.MemberProperty->NamePrivate != FName("Parent")
			|| !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return {};
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
		{
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::InvalidMetadata);
		}
		DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		auto* CandidateParent = Value ? Cast<DMaterialInterface>(Value) : nullptr;
		if (CandidateParent && CandidateParent->IsDynamicInstance())
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::ModuleRejected);
		if (Value && !CandidateParent)
		{
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::IncompatibleObject);
		}
		if (WouldCreateParentCycle(this, CandidateParent))
		{
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::ParentCycle);
		}
		return {};
	}

	auto DMaterialInstance::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		if (bDynamicInstance) return;
		RefreshParameterBindings(Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("Parent"));
		Private::RefreshMaterialDependency(*this);
		FObjectCacheContext Context;
		Super::PostEditChangePropertyWithContext(Event, Context);
		auto DirtyFlags = EMaterialRenderDirtyFlags::DynamicParameters;
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("PropertyOverrides"))
		{
			InvalidateMaterialCompilation(true, true, &Context);
			DirtyFlags = DirtyFlags | EMaterialRenderDirtyFlags::AllRenderState;
		}
		else if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("Parent"))
		{
			InvalidateMaterialCompilation(true, false, &Context);
			DirtyFlags = DirtyFlags | EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::AllRenderState;
		}
		// Publish once, after compilation invalidation has observed the completed edit.
		MarkRenderDataDirty(DirtyFlags,
			!Event.MemberProperty || Event.MemberProperty->NamePrivate != FName("PropertyOverrides"), &Context);
	}

	auto DMaterialInstance::GetParent() const -> DMaterialInterface*
	{
		return Parent.Get();
	}

	auto DMaterialInstance::GetStaticProperties() const -> const FMaterialStaticProperties&
	{
		FResolvedMaterialProperties Resolved;
		ResolvedStaticProperties = ResolveMaterialProperties(*this, Resolved)
			? Resolved.Properties : FMaterialStaticProperties{};
		return ResolvedStaticProperties;
	}


	auto DMaterialInstance::GetAcceptedCompiledProgram() const
		-> std::shared_ptr<const FMaterialProgram>
	{
		if (bDynamicInstance) return IsValid(Parent.Get()) ? Parent->GetAcceptedCompiledProgram() : nullptr;
		FResolvedMaterialProperties Resolved;
		const auto Error = ResolveMaterialProperties(*this, Resolved);
		if (!Error) return nullptr;
		return Super::GetAcceptedCompiledProgram();
	}

	auto DMaterialInstance::SetPropertyOverrides(const FMaterialPropertyOverrides& Overrides) -> bool
	{
		return SetParentAndPropertyOverrides(Parent.Get(), Overrides);
	}

	auto DMaterialInstance::SetStaticBoolOverrideByInfo(const FMaterialParameterInfo& Info, bool Value, bool bOverride) -> bool
	{
		if (bDynamicInstance || !Info.IsGlobal()) return false;
		const auto* Declaration = FindStaticBool(*this, Info, {});
		if (!Declaration) return false;
		const FMaterialStaticBoolOverride Record{Declaration->DeclarationId, Value, Info, bOverride};
		auto Found = std::ranges::find(StaticBoolOverrides, Info, &FMaterialStaticBoolOverride::ParameterInfo);
		if (Found == StaticBoolOverrides.end())
		{
			if (StaticBoolOverrides.size() >= MaterialMaxStaticBoolDeclarations) return false;
			StaticBoolOverrides.push_back(Record);
		}
		else
		{
			if (*Found == Record) return true;
			*Found = Record;
		}
		InvalidateMaterialCompilation();
		MarkPackageDirty();
		return true;
	}

	auto DMaterialInstance::ClearStaticBoolOverrideByInfo(const FMaterialParameterInfo& Info) -> bool
	{
		if (bDynamicInstance || !Info.IsGlobal()) return false;
		const auto It = std::ranges::find(StaticBoolOverrides, Info, &FMaterialStaticBoolOverride::ParameterInfo);
		if (It == StaticBoolOverrides.end()) return false;
		StaticBoolOverrides.erase(It);
		InvalidateMaterialCompilation();
		MarkPackageDirty();
		return true;
	}

	auto DMaterialInstance::ClearOrphanedStaticBoolOverride(FGuid DeclarationId) -> bool
	{
		if (bDynamicInstance || !DeclarationId.IsValid()) return false;
		const auto Before = StaticBoolOverrides.size();
		std::erase_if(StaticBoolOverrides, [&](const auto& Entry) {
			return Entry.ParameterInfo == FMaterialParameterInfo{} && Entry.DeclarationId == DeclarationId;
		});
		if (StaticBoolOverrides.size() == Before) return false;
		InvalidateMaterialCompilation();
		MarkPackageDirty();
		return true;
	}

	auto DMaterialInstance::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		FResolvedMaterialProperties Resolved;
		const auto Error = ResolveMaterialProperties(*this, Resolved);
		if (!Error) return {};
		auto* Root = Cast<DMaterialInterface>(ResolveObjectKey(Resolved.Root));
		return Root ? Root->GetParameterDefinitions() : std::span<const FMaterialParameterDefinition>{};
	}

	auto DMaterialInstance::RefreshParameterBindings(bool bParentChanged) -> void
	{
		std::unordered_set<FGuid> PreviousStaticIds;
		for (const auto& Override : StaticBoolOverrides)
			if (Override.DeclarationId.IsValid()) PreviousStaticIds.insert(Override.DeclarationId);
		for (auto& Override : StaticBoolOverrides)
		{
			const auto* Declaration = Override.DeclarationId.IsValid() ? FindStaticBool(*this, {}, Override.DeclarationId) : nullptr;
			if ((bParentChanged || !Declaration) && Override.ParameterInfo.IsGlobal())
			{
				const auto* Named = FindStaticBool(*this, Override.ParameterInfo, {});
				if (Named && !PreviousStaticIds.contains(Named->DeclarationId)) Declaration = Named;
				else if (Named && !Declaration && Override.DeclarationId.IsValid()) Override.ParameterInfo = {};
			}
			if (Declaration)
			{
				Override.DeclarationId = Declaration->DeclarationId;
				Override.ParameterInfo = {.Name = Declaration->Name};
			}
		}
		if (bDynamicInstance || GetAssetRuntimeConfiguration().RequiresCookedPayload() || !GetLocalParameterValueCount()) return;
		std::unordered_set<FGuid> PreviousIds;
		VisitParameterValueArrays([&](const auto& Records) {
			for (const auto& Record : Records)
				if (Record.ParameterId.IsValid()) PreviousIds.insert(Record.ParameterId);
		});
		const auto Definitions = GetParameterDefinitions();
		VisitParameterValueArrays([&](auto& Records) {
			for (auto& Record : Records)
			{
				const auto ById = Record.ParameterId.IsValid()
					? std::ranges::find(Definitions, Record.ParameterId, &FMaterialParameterDefinition::Id) : Definitions.end();
				auto ByName = std::ranges::find(Definitions, Record.ParameterInfo, &FMaterialParameterDefinition::GetParameterInfo);
				// An existing declaration owns its override when a rename and a stale name converge.
				if (ByName != Definitions.end() && ByName != ById && PreviousIds.contains(ByName->Id))
				{
					ByName = Definitions.end();
					if (ById == Definitions.end() && Record.ParameterId.IsValid()) Record.ParameterInfo = {};
				}
				const auto It = ByName != Definitions.end() && (bParentChanged || ById == Definitions.end()) ? ByName : ById;
				if (It != Definitions.end() && It->Value.HasSameType(Record.GetValue()))
				{
					Record.ParameterInfo = It->GetParameterInfo();
					Record.ParameterId = It->Id;
				}
			}
		});
	}

	auto DMaterialInstance::GetLocalParameterValueByInfo(const FMaterialParameterInfo& Info, FMaterialParameterValue& OutValue) const -> bool
	{
		if (!Info.IsGlobal()) return false;
		bool Found = false;
		VisitParameterValueArrays([&](const auto& Records) {
			using TRecord = typename std::decay_t<decltype(Records)>::value_type;
			const auto It = std::ranges::find(Records, Info, &TRecord::ParameterInfo);
			if (It == Records.end()) return;
			if (Found)
			{
				const auto* Definition = FindParameterDefinitionByInfo(Info);
				if (!Definition || !TRecord::SupportsType(Definition->Type)) return;
			}
			OutValue = It->GetValue();
			Found = true;
		});
		return Found;
	}

	auto DMaterialInstance::ClearParameterValueByInfo(const FMaterialParameterInfo& Info) -> bool
	{
		if (!Info.IsGlobal()) return false;
		if (bDynamicInstance)
		{
			const bool bHadValue = HasLocalParameterValueByInfo(Info);
			const std::array Updates{FMaterialDynamicParameterUpdate::ClearByInfo(Info)};
			return ApplyDynamicParameterUpdates(Updates) && bHadValue;
		}
		bool bRemoved = false;
		VisitParameterValueArrays([&](auto& Records) {
			bRemoved |= std::erase_if(Records, [&](const auto& Record) { return Record.ParameterInfo == Info; }) != 0;
		});
		if (!bRemoved) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters, true);
		return true;
	}

	auto DMaterialInstance::ResolveParameterValueByInfo(const FMaterialParameterInfo& Info, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const auto* Definition = FindParameterDefinitionByInfo(Info);
		if (!Definition) return false;
		// Property resolution already bounded and validated this chain. Walk it
		// once instead of recursively repeating declaration lookup at every layer.
		const DMaterialInterface* Owner = this;
		for (uint32 Depth = 0; Owner && Depth < MaterialMaximumParentDepth; ++Depth)
		{
			const auto* Instance = Cast<DMaterialInstance>(Owner);
			if (!Instance) return Owner->ResolveParameterValueByInfo(Info, OutParameter);
			FMaterialParameterValue LocalValue;
			if (Instance->GetLocalParameterValueByInfo(Info, LocalValue) && LocalValue.HasSameType(Definition->Value))
			{
				OutParameter.Definition = Definition;
				OutParameter.Value = std::move(LocalValue);
				OutParameter.Source = const_cast<DMaterialInstance*>(Instance);
				OutParameter.bHasLocalOverride = Instance == this;
				return true;
			}
			Owner = Instance->GetParent();
		}
		return false;
	}

	auto DMaterialInstance::SetParameterValueByInfo(const FMaterialParameterInfo& Info, const FMaterialParameterValue& Value) -> std::expected<void, std::string>
	{
		if (bDynamicInstance)
		{
			const std::array Updates{FMaterialDynamicParameterUpdate::SetByInfo(Info, Value)};
			return ApplyDynamicParameterUpdates(Updates);
		}
		const auto* Definition = FindParameterDefinitionByInfo(Info);
		if (!Definition) return std::unexpected("Material parameter does not exist: " + Info.Name.ToString());
		const auto Id = Definition->Id;
		const auto Type = Value.GetType();
		const auto Fail = [&](std::string_view Message) -> std::expected<void, std::string>
		{
			return std::unexpected(std::format("{} [parameter {}]", Message, Id.ToString()));
		};
		if (!Private::IsAuthoringMaterialParameterValue(Value)) return Fail("Material texture overrides must reference assets, not render resources.");
		if (!Definition->Value.HasSameType(Value)) return Fail("Material declaration type is invalid.");
		if (!GetParameterReachability()->ParameterNames.contains(Definition->Name)) return Fail("The parameter is not reachable from the material outputs.");
		if (!Private::IsFiniteMaterialParameterValue(Value)) return Fail("Material declaration default must be finite.");
		if (Type == EMaterialParameterType::Texture && !Value.GetTexture().IsValid())
			return Fail("Material parameter value has an invalid sampling policy.");
		FMaterialParameterValue StoredValue = Value;
		if (FMaterialVectorParameterValue::SupportsType(Type))
		{
			FMaterialVectorParameterValue Vector;
			Vector.SetValue(Value);
			StoredValue = Vector.GetValue();
		}
		if (!Private::IsFiniteMaterialParameterValue(StoredValue)) return Fail("Material declaration default must be finite.");
		FMaterialParameterValue Existing;
		if (GetLocalParameterValueByInfo(Info, Existing))
		{
			if (Existing.GetType() != Type) return Fail("The existing parameter override has a different type.");
			if (Existing == StoredValue) return {};
		}
		std::expected<void, std::string> Result;
		VisitParameterValueArrays([&](auto& Records) {
			using TRecord = typename std::decay_t<decltype(Records)>::value_type;
			if (!TRecord::SupportsType(Type)) return;
			const auto It = std::ranges::find(Records, Info, &TRecord::ParameterInfo);
			if (It != Records.end())
			{
				It->SetValue(StoredValue);
				return;
			}
			auto& Record = Records.emplace_back();
			Record.ParameterId = Id;
			Record.ParameterInfo = Info;
			Record.SetValue(StoredValue);
			// A retained orphan may still own this declaration's rename ID.
			Result = ValidateParameterStorage();
			if (!Result) Records.pop_back();
		});
		if (!Result) return Result;
		if (!bDynamicInstance) MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters, true, nullptr, std::span(&Info, 1));
		return {};
	}

	auto DMaterialInstance::ApplyDynamicParameterUpdates(
		std::span<const FMaterialDynamicParameterUpdate> Updates)
		-> std::expected<void, std::string>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		GMaterialDynamicParameterCounters.LogicalMutationCount.fetch_add(Updates.size());
		const auto Reject = [&](std::string_view Message, uint32 Index,
			FName Name) -> std::expected<void, std::string>
		{
			GMaterialDynamicParameterCounters.RejectedCommitCount.fetch_add(1);
			return std::unexpected(std::format("{} [parameter {}] [index {}]", Message, Name.ToString(), Index));
		};
		if (!bDynamicInstance)
			return Reject("The parameter expression disagrees with its declaration.", 0, {});
		if (Updates.empty())
		{
			GMaterialDynamicParameterCounters.NoOpCommitCount.fetch_add(1);
			return {};
		}
		auto Program = GetAcceptedCompiledProgram();
		if (!Program)
			for (const auto Level : {ERHIFeatureLevel::ES3_1, ERHIFeatureLevel::SM5, ERHIFeatureLevel::SM6})
				if (auto Selected = GetAcceptedCompiledProgram(Level)) { Program = std::move(Selected); break; }

		auto CandidateScalars = ScalarParameterValues;
		auto CandidateVectors = VectorParameterValues;
		auto CandidateTextures = TextureParameterValues;
		std::unordered_set<FName> SeenNames;
		std::vector<FMaterialParameterInfo> ChangedParameters;
		bool bHasClear = false;
		bool bChanged = false;
		for (uint32 Index = 0; Index < Updates.size(); ++Index)
		{
			auto Update = Updates[Index];
			const auto Info = Update.ParameterInfo;
			if (!Info.IsGlobal()) return Reject("Invalid material parameter selector.", Index, {});
			if (!SeenNames.insert(Info.Name).second)
				return Reject("Material parameter value identity is duplicated.", Index, Info.Name);
			const auto Matches = [&](const auto& Record) { return Record.ParameterInfo == Info; };
			if (Update.Operation != EMaterialDynamicParameterUpdateOperation::Set
				&& Update.Operation != EMaterialDynamicParameterUpdateOperation::Clear
				&& Update.Operation != EMaterialDynamicParameterUpdateOperation::SetTextureReference)
				return Reject("Material declaration metadata is invalid.",
					Index, Info.Name);
			if (Update.Operation == EMaterialDynamicParameterUpdateOperation::Clear)
			{
				bHasClear = true;
				bool bRemoved = false;
				const auto Erase = [&](auto& Records) {
					bRemoved |= std::erase_if(Records, Matches) != 0;
				};
				Erase(CandidateScalars);
				Erase(CandidateVectors);
				Erase(CandidateTextures);
				bChanged |= bRemoved;
				continue;
			}
			// Cooked declarations are available before program publication. Preserve
			// initialization values in the ordinary override arrays while it loads.
			if (!Program && !(GetAssetRuntimeConfiguration().RequiresCookedPayload() && IsCompiling()))
				return Reject("The parameter is not reachable from the material outputs.",
					Index, Info.Name);
			const auto* Definition = FindParameterDefinitionByInfo(Info);
			if (!Definition)
				return Reject("Material declaration does not exist.",
					Index, Info.Name);
			if (Update.Operation == EMaterialDynamicParameterUpdateOperation::SetTextureReference)
			{
				if (Definition->Type != EMaterialParameterType::Texture || Update.Value.GetType() != EMaterialParameterType::Texture
					|| Update.Value.GetTexture().Texture || !Update.Value.GetTexture().TextureReference)
					return Reject("A texture reference update requires a texture declaration and an external reference.", Index, Info.Name);
				auto Reference = std::move(Update.Value.GetTexture().TextureReference);
				Update.Value = Definition->Value;
				Update.Value.GetTexture().Texture = nullptr;
				Update.Value.GetTexture().TextureReference = std::move(Reference);
			}
			else if (!Private::IsAuthoringMaterialParameterValue(Update.Value))
				return Reject("Material texture overrides must reference assets, not render resources.", Index, Info.Name);
			FMaterialParameterValue Existing;
			if (GetLocalParameterValueByInfo(Info, Existing) && !Existing.HasSameType(Update.Value))
				return Reject("The existing parameter override has a different type.", Index, Info.Name);
			const EMaterialParameterType Type = Update.Value.GetType();
			if (Program)
			{
				bool bActive = false;
				for (const auto Level : {ERHIFeatureLevel::ES3_1, ERHIFeatureLevel::SM5, ERHIFeatureLevel::SM6})
				{
					const auto Selected = GetAcceptedCompiledProgram(Level);
					if (!Selected) continue;
					const auto Field = Selected->FindParameterField(Info);
					if (!Field) continue;
					if (!Selected->GetShaderMap().ActiveParameters[*Field].MatchesValue(Update.Value))
						return Reject("Material declaration type is invalid.", Index, Info.Name);
					bActive = true;
				}
				if (!bActive)
					return Reject("The parameter is not reachable from the material outputs.", Index, Info.Name);
			}
			if (!Definition->Value.HasSameType(Update.Value))
			{
				return Reject("Material declaration type is invalid.", Index, Info.Name);
			}
			if (!Private::IsFiniteMaterialParameterValue(Update.Value))
				return Reject("Material declaration default must be finite.",
					Index, Info.Name);
			if (Type == EMaterialParameterType::Texture
				&& !Update.Value.GetTexture().IsValid())
				return Reject("Material parameter value has an invalid sampling policy.",
					Index, Info.Name);

			FMaterialParameterValue StoredValue = Update.Value;
			if (FMaterialVectorParameterValue::SupportsType(Type))
			{
				FMaterialVectorParameterValue Vector;
				Vector.SetValue(Update.Value);
				StoredValue = Vector.GetValue();
			}
			if (!Private::IsFiniteMaterialParameterValue(StoredValue))
				return Reject("Material declaration default must be finite.",
					Index, Info.Name);
			bool bApplied = false;
			const auto Apply = [&](auto& Records) {
				using TRecord = typename std::decay_t<decltype(Records)>::value_type;
				if (!TRecord::SupportsType(Type)) return;
				auto It = std::ranges::find_if(Records, Matches);
				if (It != Records.end() && It->GetValue() == StoredValue)
				{
					bApplied = true;
					return;
				}
				if (It == Records.end()) It = Records.emplace(Records.end());
				It->ParameterInfo = Info;
				It->SetValue(StoredValue);
				bApplied = true;
				bChanged = true;
			};
			ChangedParameters.push_back(Info);
			Apply(CandidateScalars);
			Apply(CandidateVectors);
			Apply(CandidateTextures);
			if (!bApplied)
				return Reject("Material declaration type is invalid.",
					Index, Info.Name);
		}
		if (!bChanged)
		{
			GMaterialDynamicParameterCounters.NoOpCommitCount.fetch_add(1);
			return {};
		}
		ScalarParameterValues.swap(CandidateScalars);
		VectorParameterValues.swap(CandidateVectors);
		TextureParameterValues.swap(CandidateTextures);
		GMaterialDynamicParameterCounters.ChangedCommitCount.fetch_add(1);
		GMaterialDynamicParameterCounters.OwnerPublicationCount.fetch_add(1);
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters, true, nullptr,
			bHasClear ? std::span<const FMaterialParameterInfo>{} : std::span<const FMaterialParameterInfo>(ChangedParameters));
		return {};
	}

	auto DMaterialInstance::IsParameterValueOrphanByInfo(const FMaterialParameterInfo& Info) const -> bool
	{
		FMaterialParameterValue Local;
		if (!GetLocalParameterValueByInfo(Info, Local)) return false;
		const auto* Definition = FindParameterDefinitionByInfo(Info);
		if (!Definition || !Definition->Value.HasSameType(Local)) return true;
		if (!bDynamicInstance) return !GetParameterReachability()->ParameterNames.contains(Definition->Name);
		for (const auto Level : {ERHIFeatureLevel::ES3_1, ERHIFeatureLevel::SM5, ERHIFeatureLevel::SM6})
			if (auto Program = GetAcceptedCompiledProgram(Level); Program && Program->FindParameterField(Info)) return false;
		return true;
	}

	auto DMaterialInstance::SetScalarParameterValue(FName Name, float Value) -> std::expected<void, std::string>
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, EMaterialParameterType::Scalar);
		if (!Validated) return Validated;
		return SetParameterValueByInfo(Definition->GetParameterInfo(), FMaterialParameterValue::MakeScalar(Value));
	}

	auto DMaterialInstance::SetVector2ParameterValue(FName Name, const FVector2& Value) -> std::expected<void, std::string>
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, EMaterialParameterType::Vector4);
		if (!Validated) return Validated;
		return SetParameterValueByInfo(Definition->GetParameterInfo(), FMaterialParameterValue::MakeVector4(FVector4(Value, 0, 0)));
	}

	auto DMaterialInstance::SetVectorParameterValue(FName Name, const FVector3& Value) -> std::expected<void, std::string>
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, EMaterialParameterType::Vector4);
		if (!Validated) return Validated;
		return SetParameterValueByInfo(Definition->GetParameterInfo(), FMaterialParameterValue::MakeVector4(FVector4(Value, 0)));
	}

	auto DMaterialInstance::SetTextureParameterValue(FName Name, DTexture* Value) -> std::expected<void, std::string>
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, EMaterialParameterType::Texture);
		if (!Validated) return Validated;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValueByInfo(Definition->GetParameterInfo(), Resolved))
			return std::unexpected(std::format("{} [parameter {}]", "The parameter value could not be resolved.", Definition->Id.ToString()));
		Resolved.Value.GetTexture().Texture = Value;
		Resolved.Value.GetTexture().TextureReference = {};
		return SetParameterValueByInfo(Definition->GetParameterInfo(), Resolved.Value);
	}

	auto DMaterialInstance::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError>
	{
		if (bDynamicInstance || (Parent && Parent->IsDynamicInstance()))
			return RejectLoadedObjectGraph(GetObjectPath(), "Dynamic material instances cannot participate in persistent object graphs.");
		if (auto Result = Super::ValidateLoadedObjectGraph(Context); !Result) return Result;
		if (WouldCreateParentCycle(this, Parent.Get()))
			return RejectLoadedObjectGraph(GetObjectPath(), "Material instance parent chain contains a cycle.");
		if (auto Validation = ValidateMaterialStaticProperties(PropertyOverrides.Values); !Validation)
			return RejectMaterialObjectGraph(GetObjectPath(), {{.Message = Validation.error()}});
		if (Context.bCooked && CookedProgramData.GetMetadata().LogicalSize == 0)
			return RejectMaterialObjectGraph(GetObjectPath(), {{.Message = "Material cooked program is unavailable."}});
		return {};
	}

	auto DMaterialInstance::PostLoad() -> void
	{
		Super::PostLoad();
		// Break corrupt parent chains before any parameter lookup can recurse.
		if (WouldCreateParentCycle(this, Parent.Get()))
		{
			DURIN_ERROR("PostLoad '{}': material instance parent cycle; clearing parent.", GetObjectPath());
			Parent = nullptr;
		}
		RefreshParameterBindings();
		Private::RefreshMaterialDependency(*this);
		const auto Error = ValidateMaterialStaticProperties(PropertyOverrides.Values);
		if (!Error)
		{
			DURIN_ERROR("PostLoad '{}': {}; disabling static property overrides.", GetObjectPath(), Error.error());
			PropertyOverrides = {};
		}
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Private::FMaterialCompilationLifecycle::MarkCanceled(*this);
			CompilationState.SelectedResource->RequestedInput = {};
			CompilationState.SelectedResource->ClearAcceptedProgram();
			if (CookedProgramData.GetMetadata().LogicalSize == 0)
			{
				CompilationState.SelectedResource->CookDiagnostic = "Material cooked program is unavailable.";
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), CompilationState.SelectedResource->CookDiagnostic);
			}
		}
		CacheResourceShadersForRendering();
		PublishMaterialRenderProxyState();
		NotifyParameterChanges();
	}
}
