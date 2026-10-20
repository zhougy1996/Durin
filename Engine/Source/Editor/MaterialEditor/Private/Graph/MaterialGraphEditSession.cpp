#include "MaterialGraphEditSession.h"
#include "Graph/MaterialGraphChange.h"
#include "Materials/Material.h"
#include "DObject/Class.h"
#include "DObject/Property.h"
#include "DObject/Package.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/MaterialExpressionBuild.h"

namespace Durin::Editor::Material::GraphEditInternals
{
	namespace
	{
		struct FMemberChange
		{
			FPersistentObjectRef Target;
			FProperty* Member = nullptr;
			FName MemberName;
			uint32 ElementIndex = 0;
			FPropertyValueSnapshotPayload Before, After;
			auto Object() const -> DObject* { return Target.Resolve(); }
			auto Property() const -> FProperty*
			{
				auto* Owner = Object();
				return Owner && Owner->GetClass()->FindPropertyByName(MemberName) == Member ? Member : nullptr;
			}
			auto Index() const -> uint32 { return ElementIndex; }
			auto Capture(DObject& Object, FProperty* Property, uint32 Index) -> std::expected<void, FPropertySnapshotError>
			{
				Target = FPersistentObjectRef(&Object); Member = Property; ElementIndex = Index;
				MemberName = Property ? Property->NamePrivate : FName();
				return CapturePropertyValuePayload(this->Property(), &Object, Index, Before);
			}
			auto CaptureAfter() -> std::expected<void, FPropertySnapshotError>
			{ return CapturePropertyValuePayload(Property(), Object(), Index(), After); }
			auto IsNoOp() const -> bool { return Before == After; }
			auto Restore(bool bBefore) const -> FMaterialChangeResult
			{
				if (!Property()) return {{.Code = EMaterialChangeError::MemberUnavailable}};
				const auto Restored = RestorePropertyValuePayload(Property(), Object(), Index(), bBefore ? Before : After);
				if (!Restored) return {{.Code = EMaterialChangeError::PropertyRestore,
					.PropertyCause = std::make_shared<FPropertySnapshotError>(Restored.error())}};
				return {};
			}
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void
			{
				Target.AddReferencedObjects(Collector);
				for (const auto* Payload : {&Before, &After})
					for (auto Key : Payload->GetReferencedObjectKeys()) FPersistentObjectRef::FromKey(Key).AddReferencedObjects(Collector);
			}
		};
		using FNodes = std::vector<TStrongObjectPtr<DMaterialExpression>>;
		using FNodeReferences = std::vector<TObjectPtr<DMaterialExpression>>;
		auto ReadPresentation(DObject& Owner) -> FMaterialGraphPresentation
		{
			if (auto* M = Cast<DMaterial>(&Owner)) return M->GetMaterialGraphPresentation();
			return {.Nodes = Cast<DMaterialFunction>(&Owner)->GetFunctionPresentation().Nodes};
		}
		auto WritePresentation(DObject& Owner, const FMaterialGraphPresentation& Value) -> void
		{
			if (auto* M = Cast<DMaterial>(&Owner)) M->SetMaterialGraphPresentation(Value);
			else Cast<DMaterialFunction>(&Owner)->SetFunctionPresentation({.Nodes = Value.Nodes});
		}
		template<class TNodes>
		auto SetNodes(DObject& Owner, const TNodes& Nodes) -> void
		{
			auto& Current = FMaterialExpressionEditing::GetExpressions(Owner);
			std::vector<DMaterialExpression*> Previous;
			for (auto& E : Current) Previous.push_back(E.Get());
			Current.clear();
			for (auto& E : Nodes) Current.emplace_back(E.Get());
			FMaterialExpressionEditing::ReconcileOwnership(Owner, Previous);
		}
		class FGraphObjectChange final : public FMaterialCommandChange
		{
		public:
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{
				std::vector<DObject*> Objects{Owner.Get()};
				for (const auto& Member : Members) Objects.push_back(Member.Object());
				if (bStructural)
				{
					for (const auto& Node : BeforeNodes)
						if (!std::ranges::contains(AfterNodes, Node)) Objects.push_back(Node.Get());
					for (const auto& Node : AfterNodes)
						if (!std::ranges::contains(BeforeNodes, Node)) Objects.push_back(Node.Get());
				}
				return Objects;
			}

			TWeakObjectPtr<DObject> Owner;
			std::vector<FMemberChange> Members;
			FNodeReferences BeforeNodes, AfterNodes; // Collector-owned structural membership.
			bool bStructural = false;
			bool bPresentation = false;
			FMaterialGraphPresentation BeforePresentation, AfterPresentation;
			std::string Description;
			std::array<DPackage*, 1> Packages;
			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetOwningModule() const -> std::string_view override { return "MaterialEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto Replay(ETransactionOperation Operation) -> FMaterialChangeResult override
			{ return Apply(Operation == ETransactionOperation::Undo); }
			auto Apply(bool bBefore) -> FMaterialChangeResult
			{
				if (!Owner.IsValid()) return {{.Code = EMaterialChangeError::TargetUnavailable}};
				for (size_t Index = 0; Index < Members.size(); ++Index)
					if (!Members[Index].Object()) return {{.Code = EMaterialChangeError::MemberUnavailable, .MemberIndex = Index}};
				const auto& Current = FMaterialExpressionEditing::GetExpressions(*Owner.Get());
				if (bStructural)
				{
					if (Current != (bBefore ? AfterNodes : BeforeNodes)) return {{.Code = EMaterialChangeError::MembershipChanged, .NodeCount = Current.size()}};
				}
				else
				{
					// An external bulk replacement must not turn an old object record
					// into a successful edit of detached, no-longer-visible expressions.
					for (const auto& M : Members)
						if (std::ranges::none_of(Current, [&](auto& E) { return E.Get() == M.Object(); })) return {{.Code = EMaterialChangeError::MembershipChanged, .NodeCount = Current.size()}};
				}
				// Restore all fields before deriving owner state or notifying observers.
				size_t Applied = 0;
				for (const auto& M : Members)
				{
					if (auto Restored = M.Restore(bBefore); !Restored)
					{
						while (Applied)
						{
							const auto& R = Members[--Applied];
							const auto bRestored = R.Restore(!bBefore);
							if (!bRestored) std::terminate();
						}
						Restored.Error.MemberIndex = static_cast<size_t>(&M - Members.data());
						return Restored;
					}
					++Applied;
				}
				FScopedMaterialGraphChange Batch(*Owner.Get());
				if (bStructural) SetNodes(*Owner.Get(), bBefore ? BeforeNodes : AfterNodes);
				if (bPresentation) WritePresentation(*Owner.Get(), bBefore ? BeforePresentation : AfterPresentation);
				if (bStructural || !Members.empty()) FMaterialExpressionEditing::Publish(*Owner.Get());
				return {};
			}
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (const auto& M : Members)
				{
					M.AddReferencedObjects(Collector);
				}
				for (const auto* Nodes : {&BeforeNodes, &AfterNodes})
					for (auto& E : *Nodes) { DObject* Object = E.Get(); Collector.AddReferencedObject(Object); }
			}
			auto GetAllocatedSize() const -> size_t override
			{
				size_t Bytes = Description.capacity() + Members.capacity() * sizeof(FMemberChange)
					+ (BeforeNodes.capacity() + AfterNodes.capacity()) * sizeof(FNodeReferences::value_type);
				for (const auto& M : Members)
					for (const auto* Payload : {&M.Before, &M.After}) { size_t Size = 0; if (!Payload->TryGetAllocatedSize(Size)) return std::numeric_limits<size_t>::max(); Bytes += Size; }
				for (const auto* P : {&BeforePresentation, &AfterPresentation})
				{
					Bytes += P->Nodes.capacity() * sizeof(FMaterialGraphNodePresentation);
					for (const auto& N : P->Nodes) Bytes += N.DisplayName.capacity();
				}
				return Bytes;
			}
		};
	}

	struct FGraphEditSession::FImpl
	{
		DObject& Owner;
		FNodes Original;
		std::unordered_set<DMaterialExpression*> OriginalObjects;
		FMaterialGraphPresentation OriginalPresentation;
		std::vector<FMemberChange> Members;
		std::unordered_set<DMaterialExpression*> Modified;
		bool bFinished = false;
		std::shared_ptr<const FPropertySnapshotError> CaptureError;
	};
	FGraphEditSession::FGraphEditSession(DObject& Owner)
		: bFunction(Cast<DMaterialFunction>(&Owner) != nullptr),
		Expressions(FMaterialExpressionEditing::GetExpressions(Owner)), Presentation(ReadPresentation(Owner)),
		Impl(std::make_unique<FImpl>(Owner))
	{
		Impl->OriginalPresentation = Presentation;
		for (auto& E : Expressions) { Impl->Original.emplace_back(E.Get()); Impl->OriginalObjects.insert(E.Get()); }
	}
	FGraphEditSession::~FGraphEditSession()
	{
		if (Impl->bFinished) return;
		for (const auto& M : Impl->Members)
			if (!M.Restore(true)) std::terminate();
		SetNodes(Impl->Owner, Impl->Original);
	}
	auto FGraphEditSession::Modify(DMaterialExpression& Expression) -> void
	{
		if (!Impl->Modified.insert(&Expression).second) return;
		// New objects are retained by structural history; they have no prior authored state.
		if (!Impl->OriginalObjects.contains(&Expression)) return;
		Expression.GetClass()->ForEachProperty([&](FProperty* P) {
			if (P->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < P->GetArrayDim(); ++Index)
			{
				FMemberChange M;
				const auto Captured = M.Capture(Expression, P, Index);
				if (Captured) Impl->Members.push_back(std::move(M));
				else if (!Impl->CaptureError) Impl->CaptureError = std::make_shared<FPropertySnapshotError>(Captured.error());
			}
		});
	}
	auto FGraphEditSession::Assign(DMaterialExpression& Target, const DMaterialExpression& Source) -> FMaterialGraphCommandResult
	{
		if (Target.GetClass() != Source.GetClass()) return RejectCommand("Expression assignment requires matching classes.");
		FMaterialGraphCommandResult Result;
		Target.GetClass()->ForEachProperty([&](FProperty* P) {
			if (P->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 Index = 0; Index < P->GetArrayDim(); ++Index)
			{
				if (ArePropertyValuesIdentical(P, &Source, Index, &Target, Index)) continue;
				Modify(Target);
				const auto Copied = P->CopyAssignValue(P->GetValuePtr(&Target, Index), P->GetValuePtr(&Source, Index));
				if (!Copied && Result) Result = RejectCommand("Unable to update the expression properties. " + ToString(Copied.error()));
			}
		});
		return Result;
	}
	auto FGraphEditSession::GetOutputs() -> FMaterialExpressionSurfaceOutputs&
	{
		for (auto& E : Expressions)
			if (auto* Output = Cast<DMaterialExpressionMaterialOutput>(E.Get())) { Modify(*Output); return Output->Outputs; }
		check(false); std::terminate();
	}
	auto FGraphEditSession::Commit(std::string Description, DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Transactions && !Transactions->CanTransact()) return RejectCommand("The editor transactor is busy.");
		const bool bGraphWritten = !Impl->Members.empty() || !std::ranges::equal(Expressions, Impl->Original,
			{}, [](auto& E) { return E.Get(); }, [](auto& E) { return E.Get(); });
		if (bGraphWritten)
		{
			const auto Validation = FMaterialExpressionEditing::ValidateStorage(Impl->Owner);
			if (!Validation) return RejectCommand("The expression storage is invalid.", Validation.Diagnostics);
			std::unordered_set<FGuid> Ids;
			for (auto& E : Expressions) Ids.insert(E->Id);
			bool bMissingSource = false;
			for (auto& E : Expressions) VisitMaterialExpressionInputs(*E, [&](uint32, const FMaterialExpressionInput& Input) {
				bMissingSource |= Input.ExpressionId.IsValid() && !Ids.contains(Input.ExpressionId);
			});
			if (bMissingSource) return RejectCommand("A connection refers to a missing expression.", {{.Error = EMaterialExpressionError::InputDisconnectedRefersMissingExpression}});
			if (bFunction)
			{
				std::vector<DMaterialFunctionInterface*> Roots;
				for (auto& E : Expressions) if (auto* Call = Cast<DMaterialExpressionFunctionCall>(E.Get())) Roots.push_back(Call->Function.Get());
				std::vector<FMaterialFunctionOwnerStamp> Closure;
				if (std::ranges::any_of(Roots, [&](auto* Root) { return Root == &Impl->Owner; }))
					return RejectCommand("This edit would introduce recursive function dependencies.");
				const auto Dependencies = ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing);
				if (!Dependencies) return RejectCommand("The function dependencies are invalid.", Dependencies.Diagnostics);
				if (std::ranges::any_of(Closure, [&](const auto& D) { return D.AssetPath == Impl->Owner.GetObjectPath(); }))
					return RejectCommand("This edit would introduce recursive function dependencies.");
			}
			// Type inference is best effort. Incompatible widths remain compiler diagnostics.
			std::unordered_set<FGuid> TypeChanges, ChangedOutputs;
			for (auto& M : Impl->Members)
			{
				if (const auto Captured = M.CaptureAfter(); !Captured)
					return RejectCommand("Unable to inspect the edited expression. " + ToString(Captured.error()));
				if (M.IsNoOp()) continue;
				auto* E = Cast<DMaterialExpression>(M.Object());
				// Constant values never change their class-defined output width.
				// Other fields are conservative seeds: defaults can constrain math widths.
				if (M.Property()->NamePrivate == FName("Value")
					&& (Cast<DMaterialExpressionScalarConstant>(E) || Cast<DMaterialExpressionVector2Constant>(E)
						|| Cast<DMaterialExpressionVector3Constant>(E) || Cast<DMaterialExpressionVector4Constant>(E))) continue;
				TypeChanges.insert(E->Id);
				if (M.Property()->NamePrivate == FName("ResultType")) ChangedOutputs.insert(E->Id);
			}
			for (auto& E : Expressions)
				if (!Impl->OriginalObjects.contains(E.Get())) { TypeChanges.insert(E->Id); ChangedOutputs.insert(E->Id); }
			if (!TypeChanges.empty())
			{
				const std::vector<FGuid> Seeds(TypeChanges.begin(), TypeChanges.end());
				const std::vector<FGuid> Outputs(ChangedOutputs.begin(), ChangedOutputs.end());
				AdaptNumericTypes(*this, Seeds, Outputs);
			}
			if (Impl->CaptureError) return RejectCommand("Unable to record the edited expression. " + ToString(*Impl->CaptureError));
			if (!bFunction) std::stable_partition(Expressions.begin(), Expressions.end(), [](auto& E) { return !Cast<DMaterialExpressionMaterialOutput>(E.Get()); });
		}
		std::vector<FGuid> PresentationIds;
		for (auto& E : Expressions) PresentationIds.push_back(E->Id);
		Presentation = SanitizeMaterialGraphPresentation(Presentation, PresentationIds);
		auto Change = std::make_unique<FGraphObjectChange>();
		Change->Owner = &Impl->Owner; Change->Description = std::move(Description); Change->Packages = {Impl->Owner.GetPackage()};
		for (auto& M : Impl->Members)
		{
			if (const auto Captured = M.CaptureAfter(); !Captured)
				return RejectCommand("Unable to record the edited expression. " + ToString(Captured.error()));
			if (!M.IsNoOp()) Change->Members.push_back(M);
		}
		Change->bStructural = !std::ranges::equal(Expressions, Impl->Original, {}, [](auto& E) { return E.Get(); }, [](auto& E) { return E.Get(); });
		if (Change->bStructural)
		{
			for (auto& E : Impl->Original) Change->BeforeNodes.emplace_back(E.Get());
			for (auto& E : Expressions) Change->AfterNodes.emplace_back(E.Get());
			// Creation/deletion history restores the object's fields as well as its
			// membership, even if a caller retained and modified the detached object.
			for (const auto* Side : {&Change->BeforeNodes, &Change->AfterNodes})
			{
				const auto& Other = Side == &Change->BeforeNodes ? Change->AfterNodes : Change->BeforeNodes;
				for (auto& E : *Side)
				{
					if (std::ranges::any_of(Other, [&](auto& O) { return O.Get() == E.Get(); })) continue;
					E->GetClass()->ForEachProperty([&](FProperty* P) {
						if (P->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
						for (uint32 Index = 0; Index < P->GetArrayDim(); ++Index)
						{
							if (std::ranges::any_of(Change->Members, [&](const auto& M) { return M.Object() == E.Get() && M.Property() == P && M.Index() == Index; })) continue;
							FMemberChange M;
							if (const auto Captured = M.Capture(*E, P, Index); !Captured)
							{
								if (!Impl->CaptureError) Impl->CaptureError = std::make_shared<FPropertySnapshotError>(Captured.error());
								continue;
							}
							M.After = M.Before;
							Change->Members.push_back(std::move(M));
						}
					});
				}
			}
		}
		if (Impl->CaptureError) return RejectCommand("Unable to record the structural edit. " + ToString(*Impl->CaptureError));
		if (!Change->bStructural && Change->Members.empty() && Presentation == Impl->OriginalPresentation)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		Change->bPresentation = Presentation != Impl->OriginalPresentation;
		if (Change->bPresentation)
		{
			Change->BeforePresentation = Impl->OriginalPresentation;
			Change->AfterPresentation = Presentation;
		}
		const bool bSemantic = Change->bStructural || !Change->Members.empty();
		FScopedTransaction Recording(Transactions, {"MaterialGraph", std::string(Change->GetDescription())});
		// Record before publishing dirty/revision/compile effects. A rejected history
		// commit leaves the scope responsible for restoring the original live fields.
		if (Transactions)
		{
			const auto Recorded = Transactions->StoreUndo(&Impl->Owner, std::move(Change));
			if (!Recorded) return RejectCommand("Unable to record the graph edit. " + FormatTransactorResult(Recorded));
		}
		std::vector<DMaterialExpression*> Previous;
		for (auto& E : Impl->Original) Previous.push_back(E.Get());
		FMaterialExpressionEditing::ReconcileOwnership(Impl->Owner, Previous);
		Impl->bFinished = true;
		FScopedMaterialGraphChange Batch(Impl->Owner);
		WritePresentation(Impl->Owner, Presentation);
		if (bSemantic) FMaterialExpressionEditing::Publish(Impl->Owner);
		(void)Recording.End();
		if (Transactions && Transactions->GetLastResult().Code == ETransactorResultCode::Failed)
			return RejectCommand(FormatTransactorResult(Transactions->GetLastResult()));
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}
}
