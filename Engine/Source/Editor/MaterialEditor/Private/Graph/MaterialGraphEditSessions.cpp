#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"
#include "Materials/MaterialExpressionEditing.h"

#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	namespace
	{
		auto GraphSemanticRevision(const DObject& Owner) -> uint64
		{
			if (const auto* Material = Cast<DMaterial>(&Owner)) return Material->GetMaterialCompileStatus().AuthoredRevision;
			return Cast<DMaterialFunction>(&Owner)->GetFunctionRevision();
		}
	}

	struct FMaterialGraphMoveSession::FImpl
	{
		TWeakObjectPtr<DObject> Owner;
		std::unordered_set<FGuid> NodeIds;
		std::vector<FMaterialGraphNodePresentation> Draft;
		DTransactor* Transactions = nullptr;
		uint64 AuthoredRevision = 0;
		bool bActive = false;
	};

	FMaterialGraphMoveSession::FMaterialGraphMoveSession() : Impl(std::make_unique<FImpl>()) {}
	FMaterialGraphMoveSession::~FMaterialGraphMoveSession() = default;

	auto FMaterialGraphMoveSession::Begin(DObject& Owner, std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Impl->bActive) return RejectSession({.Code = EMaterialGraphSessionError::MoveActive});
		if (!IsValid(&Owner)) return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner});
		if (!Cast<DMaterial>(&Owner) && !Cast<DMaterialFunction>(&Owner)) return RejectSession({.Code = EMaterialGraphSessionError::OwnerType, .TargetClass = Owner.GetClass()->GetName()});
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return RejectSession({.Code = EMaterialGraphSessionError::SelectionBounds, .ActualCount = NodeIds.size(), .Limit = MaterialProgramMaxNodeCount});
		if (Transactions && Transactions->HasPendingOperation()) return RejectSession({.Code = EMaterialGraphSessionError::Busy});
		Impl->NodeIds.clear();
		const auto& Expressions = FMaterialExpressionEditing::GetExpressions(Owner);
		for (const auto& Id : NodeIds)
		{
			if (std::ranges::none_of(Expressions, [&](const auto& E) { return E->Id == Id; }))
				return RejectSession({.Code = EMaterialGraphSessionError::SelectionNode, .TargetNodeId = Id});
			if (!Impl->NodeIds.insert(Id).second) return RejectSession({.Code = EMaterialGraphSessionError::SelectionDuplicate, .TargetNodeId = Id});
		}
		Impl->Owner = &Owner;
		Impl->Draft.clear();
		Impl->Transactions = Transactions;
		Impl->AuthoredRevision = GraphSemanticRevision(Owner);
		Impl->bActive = true;
		return {.Disposition = EMaterialGraphCommandDisposition::Applied,
			.AffectedNodeIds = std::vector<FGuid>(NodeIds.begin(), NodeIds.end())};
	}

	auto FMaterialGraphMoveSession::IsCurrent(const DObject& Owner) const -> bool
	{
		return Impl->bActive && Impl->Owner.Get() == &Owner && GraphSemanticRevision(Owner) == Impl->AuthoredRevision;
	}

	auto FMaterialGraphMoveSession::Apply(std::span<const FMaterialGraphNodePresentation> Positions)
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectSession({.Code = EMaterialGraphSessionError::MoveInactive});
		if (!Impl->Owner.IsValid()) { Cancel(); return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner}); }
		if (!IsCurrent(*Impl->Owner.Get())) { Cancel(); return RejectSession({.Code = EMaterialGraphSessionError::MoveRevision, .ExpectedRevision = Impl->AuthoredRevision, .ActualRevision = GraphSemanticRevision(*Impl->Owner.Get())}); }
		if (Positions.size() > MaterialProgramMaxNodeCount) return RejectSession({.Code = EMaterialGraphSessionError::PreviewBounds, .ActualCount = Positions.size(), .Limit = MaterialProgramMaxNodeCount});
		std::unordered_set<FGuid> Requested;
		for (const auto& Position : Positions)
		{
			if (!Impl->NodeIds.contains(Position.NodeId)) return RejectSession({.Code = EMaterialGraphSessionError::PreviewSelection, .TargetNodeId = Position.NodeId});
			if (!Requested.insert(Position.NodeId).second) return RejectSession({.Code = EMaterialGraphSessionError::PreviewDuplicate, .TargetNodeId = Position.NodeId});
			if (Position.X < -MaterialGraphPresentationCoordinateLimit || Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit || Position.Y > MaterialGraphPresentationCoordinateLimit)
				return RejectSession({.Code = EMaterialGraphSessionError::PreviewCoordinate, .TargetNodeId = Position.NodeId, .X = Position.X, .Y = Position.Y, .CoordinateLimit = MaterialGraphPresentationCoordinateLimit});
		}
		bool bChanged = false;
		for (const auto& Position : Positions)
		{
			auto It = std::ranges::find(Impl->Draft, Position.NodeId, &FMaterialGraphNodePresentation::NodeId);
			if (It == Impl->Draft.end()) { Impl->Draft.push_back({Position.NodeId, Position.X, Position.Y}); bChanged = true; }
			else { bChanged |= It->X != Position.X || It->Y != Position.Y; It->X = Position.X; It->Y = Position.Y; }
		}
		return {.Disposition = bChanged ? EMaterialGraphCommandDisposition::Applied : EMaterialGraphCommandDisposition::NoChange,
			.AffectedNodeIds = std::vector<FGuid>(Requested.begin(), Requested.end())};
	}

	auto FMaterialGraphMoveSession::Commit() -> FMaterialGraphCommandResult
	{
		const auto Validated = Apply({});
		if (!Validated) return Validated;
		auto Result = FMaterialGraphDocument(*Impl->Owner.Get()).MoveNodes(Impl->Draft, Impl->Transactions);
		if (Result) { Impl->bActive = false; Impl->Draft.clear(); }
		return Result;
	}

	auto FMaterialGraphMoveSession::Cancel() -> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectSession({.Code = EMaterialGraphSessionError::MoveInactive});
		Impl->bActive = false;
		Impl->Draft.clear();
		return {.Disposition = EMaterialGraphCommandDisposition::Applied};
	}

	auto FMaterialGraphMoveSession::IsActive() const -> bool { return Impl->bActive; }
	auto FMaterialGraphMoveSession::GetPositions() const -> std::span<const FMaterialGraphNodePresentation> { return Impl->Draft; }

	struct FMaterialGraphParameterEditSession::FImpl
	{
		TWeakObjectPtr<DMaterial> Material;
		FGuid ParameterId;
		FMaterialParameterValue BeforeValue;
		FMaterialParameterValue CurrentValue;
		TStrongObjectPtr<DTexture2D> BeforeTexture;
		TStrongObjectPtr<DTexture2D> CurrentTexture;
		DTransactor* Transactions = nullptr;
		bool bActive = false;
	};

	FMaterialGraphParameterEditSession::FMaterialGraphParameterEditSession()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FMaterialGraphParameterEditSession::~FMaterialGraphParameterEditSession()
	{
		if (Impl && Impl->bActive) Cancel();
	}

	auto FMaterialGraphParameterEditSession::Begin(
		DMaterial& Material,
		const FGuid& ParameterId,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Impl->bActive) return RejectSession({.Code = EMaterialGraphSessionError::ParameterActive, .ParameterId = ParameterId, .ActiveParameterId = Impl->ParameterId});
		if (!IsValid(&Material))
			return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner, .ParameterId = ParameterId});
		if (Transactions && Transactions->HasPendingOperation())
			return RejectSession({.Code = EMaterialGraphSessionError::Busy, .ParameterId = ParameterId});
		FResolvedMaterialParameter Resolved;
		if (!Material.ResolveParameterValue(ParameterId, Resolved)
			|| !Resolved.Definition)
			return RejectSession({.Code = EMaterialGraphSessionError::ParameterDefinition, .ParameterId = ParameterId});
		Impl->Material = &Material;
		Impl->ParameterId = ParameterId;
		Impl->BeforeValue = Resolved.Value;
		Impl->CurrentValue = Resolved.Value;
		Impl->BeforeTexture = Resolved.Value.GetType() == EMaterialParameterType::Texture
			? Resolved.Value.GetTexture().Texture.Get() : nullptr;
		Impl->CurrentTexture = Impl->BeforeTexture;
		Impl->Transactions = Transactions;
		Impl->bActive = true;
		return {.Disposition = EMaterialGraphCommandDisposition::Applied};
	}

	auto FMaterialGraphParameterEditSession::Apply(FMaterialParameterValue Value)
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectSession({.Code = EMaterialGraphSessionError::ParameterInactive, .ParameterId = Impl->ParameterId});
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner, .ParameterId = Impl->ParameterId});
		}
		if (Value == Impl->CurrentValue)
			return {.Disposition = EMaterialGraphCommandDisposition::NoChange};
		FScopedPackageDirtySuppression SuppressPackageRevision;
		if (const auto Applied = Material->SetParameterValue(Impl->ParameterId, Value); !Applied)
			return RejectSession({.Code = EMaterialGraphSessionError::ParameterValue, .MaterialCause = Applied.Error, .ParameterId = Impl->ParameterId,
				.RequestedType = Value.GetType(), .ExistingType = Impl->CurrentValue.GetType()});
		Impl->CurrentValue = std::move(Value);
		Impl->CurrentTexture = Impl->CurrentValue.GetType() == EMaterialParameterType::Texture
			? Impl->CurrentValue.GetTexture().Texture.Get() : nullptr;
		std::vector<FGuid> AffectedNodes;
		for (const auto& Expression : Material->GetExpressionCollection().Expressions)
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()); Parameter && Parameter->Metadata.Id == Impl->ParameterId)
				AffectedNodes.push_back(Expression->Id);
		return {.Disposition = EMaterialGraphCommandDisposition::Applied,
			.AffectedNodeIds = std::move(AffectedNodes)};
	}

	auto FMaterialGraphParameterEditSession::Commit()
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectSession({.Code = EMaterialGraphSessionError::ParameterInactive, .ParameterId = Impl->ParameterId});
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner, .ParameterId = Impl->ParameterId});
		}
		if (Impl->Transactions && Impl->Transactions->HasPendingOperation())
			return RejectSession({.Code = EMaterialGraphSessionError::Busy, .ParameterId = Impl->ParameterId});
		if (Impl->BeforeValue.GetType() == EMaterialParameterType::Texture)
		{
			Impl->BeforeValue.GetTexture().Texture = Impl->BeforeTexture.Get();
			Impl->CurrentValue.GetTexture().Texture = Impl->CurrentTexture.Get();
		}
		const bool bChanged = Impl->BeforeValue != Impl->CurrentValue;
		if (bChanged && Impl->Transactions)
		{
			const auto Recorded = Impl->Transactions->CommitApplied(
				MakeMaterialGraphParameterTransaction(
					*Material, Impl->ParameterId,
					Impl->BeforeValue, Impl->CurrentValue));
			if (!Recorded) return RejectSession({.Code = EMaterialGraphSessionError::History,
				.TransactorCause = std::make_shared<FTransactorResult>(Recorded), .ParameterId = Impl->ParameterId});
		}
		if (bChanged) Material->MarkPackageDirty();
		Impl->BeforeTexture.Reset();
		Impl->CurrentTexture.Reset();
		Impl->bActive = false;
		return {.Disposition = bChanged ? EMaterialGraphCommandDisposition::Applied
			: EMaterialGraphCommandDisposition::NoChange};
	}

	auto FMaterialGraphParameterEditSession::Cancel()
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectSession({.Code = EMaterialGraphSessionError::ParameterInactive, .ParameterId = Impl->ParameterId});
		DMaterial* Material = Impl->Material.Get();
		Impl->bActive = false;
		if (Impl->BeforeValue.GetType() == EMaterialParameterType::Texture)
			Impl->BeforeValue.GetTexture().Texture = Impl->BeforeTexture.Get();
		if (!Material)
			return RejectSession({.Code = EMaterialGraphSessionError::StaleOwner, .ParameterId = Impl->ParameterId});
		const bool bChanged = Impl->BeforeValue != Impl->CurrentValue;
		if (bChanged)
		{
			FScopedPackageDirtySuppression SuppressPackageRevision;
			if (const auto Restored = Material->SetParameterValue(
				Impl->ParameterId, Impl->BeforeValue); !Restored)
				return RejectSession({.Code = EMaterialGraphSessionError::ParameterRestore, .MaterialCause = Restored.Error, .ParameterId = Impl->ParameterId,
					.RequestedType = Impl->BeforeValue.GetType(), .ExistingType = Impl->CurrentValue.GetType()});
		}
		Impl->BeforeTexture.Reset();
		Impl->CurrentTexture.Reset();
		return {.Disposition = bChanged ? EMaterialGraphCommandDisposition::Applied
			: EMaterialGraphCommandDisposition::NoChange};
	}

	auto FMaterialGraphParameterEditSession::IsActive() const -> bool
	{
		return Impl->bActive;
	}
}
