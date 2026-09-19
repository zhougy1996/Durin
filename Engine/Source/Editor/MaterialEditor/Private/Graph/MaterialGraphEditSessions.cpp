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
		if (Impl->bActive) return RejectCommand("A graph move is already active.");
		if (!IsValid(&Owner)) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (!Cast<DMaterial>(&Owner) && !Cast<DMaterialFunction>(&Owner)) return RejectCommand("Unsupported graph owner.");
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return RejectCommand("The graph move selection is empty or exceeds the node bound.");
		if (Transactions && Transactions->HasPendingOperation()) return RejectCommand("The editor transactor is busy.");
		Impl->NodeIds.clear();
		const auto& Expressions = FMaterialExpressionEditing::GetExpressions(Owner);
		for (const auto& Id : NodeIds)
		{
			if (std::ranges::none_of(Expressions, [&](const auto& E) { return E->Id == Id; }))
				return RejectCommand("A selected graph node does not exist.");
			if (!Impl->NodeIds.insert(Id).second) return RejectCommand("The graph move selection contains a duplicate node GUID.");
		}
		Impl->Owner = &Owner;
		Impl->Draft.clear();
		Impl->Transactions = Transactions;
		Impl->AuthoredRevision = GraphSemanticRevision(Owner);
		Impl->bActive = true;
		return {.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::vector<FGuid>(NodeIds.begin(), NodeIds.end())};
	}

	auto FMaterialGraphMoveSession::IsCurrent(const DObject& Owner) const -> bool
	{
		return Impl->bActive && Impl->Owner.Get() == &Owner && GraphSemanticRevision(Owner) == Impl->AuthoredRevision;
	}

	auto FMaterialGraphMoveSession::Apply(std::span<const FMaterialGraphNodePresentation> Positions)
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectCommand("No graph move is active.");
		if (!Impl->Owner.IsValid()) { Cancel(); return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner); }
		if (!IsCurrent(*Impl->Owner.Get())) { Cancel(); return RejectCommand("The graph changed semantically during the move."); }
		if (Positions.size() > MaterialProgramMaxNodeCount) return RejectCommand("The graph move preview exceeds the node bound.");
		std::unordered_set<FGuid> Requested;
		for (const auto& Position : Positions)
		{
			if (!Impl->NodeIds.contains(Position.NodeId)) return RejectCommand("The graph move preview addresses a node outside the selection.");
			if (!Requested.insert(Position.NodeId).second) return RejectCommand("The graph move preview contains a duplicate node GUID.");
			if (Position.X < -MaterialGraphPresentationCoordinateLimit || Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit || Position.Y > MaterialGraphPresentationCoordinateLimit)
				return RejectCommand("The graph move preview is outside the supported coordinate range.");
		}
		bool bChanged = false;
		for (const auto& Position : Positions)
		{
			auto It = std::ranges::find(Impl->Draft, Position.NodeId, &FMaterialGraphNodePresentation::NodeId);
			if (It == Impl->Draft.end()) { Impl->Draft.push_back({Position.NodeId, Position.X, Position.Y}); bChanged = true; }
			else { bChanged |= It->X != Position.X || It->Y != Position.Y; It->X = Position.X; It->Y = Position.Y; }
		}
		return {.Status = bChanged ? EMaterialGraphCommandStatus::Succeeded : EMaterialGraphCommandStatus::NoChange,
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
		if (!Impl->bActive) return RejectCommand("No graph move is active.");
		Impl->bActive = false;
		Impl->Draft.clear();
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
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
		if (Impl->bActive) return RejectCommand("A material parameter edit is already active.");
		if (!IsValid(&Material))
			return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (Transactions && Transactions->HasPendingOperation())
			return RejectCommand("The editor transactor is busy.");
		FResolvedMaterialParameter Resolved;
		if (!Material.ResolveParameterValue(ParameterId, Resolved)
			|| !Resolved.Definition)
			return RejectCommand("The material parameter definition is unavailable.");
		Impl->Material = &Material;
		Impl->ParameterId = ParameterId;
		Impl->BeforeValue = Resolved.Value;
		Impl->CurrentValue = Resolved.Value;
		Impl->BeforeTexture = Resolved.Value.GetType() == EMaterialParameterType::Texture
			? Resolved.Value.GetTexture().Texture.Get() : nullptr;
		Impl->CurrentTexture = Impl->BeforeTexture;
		Impl->Transactions = Transactions;
		Impl->bActive = true;
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}

	auto FMaterialGraphParameterEditSession::Apply(FMaterialParameterValue Value)
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectCommand("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		}
		if (Value == Impl->CurrentValue)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		FScopedPackageDirtySuppression SuppressPackageRevision;
		if (const auto Applied = Material->SetParameterValue(Impl->ParameterId, Value); !Applied)
			return RejectCommand("The material rejected the parameter value. " + FormatMaterialError(Applied.Error));
		Impl->CurrentValue = std::move(Value);
		Impl->CurrentTexture = Impl->CurrentValue.GetType() == EMaterialParameterType::Texture
			? Impl->CurrentValue.GetTexture().Texture.Get() : nullptr;
		std::vector<FGuid> AffectedNodes;
		for (const auto& Expression : Material->GetExpressionCollection().Expressions)
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()); Parameter && Parameter->Metadata.Id == Impl->ParameterId)
				AffectedNodes.push_back(Expression->Id);
		return {.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(AffectedNodes)};
	}

	auto FMaterialGraphParameterEditSession::Commit()
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectCommand("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		}
		if (Impl->Transactions && Impl->Transactions->HasPendingOperation())
			return RejectCommand("The editor transactor is busy.");
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
			if (!Recorded) return RejectCommand("Unable to record the graph edit. " + FormatTransactorResult(Recorded));
		}
		if (bChanged) Material->MarkPackageDirty();
		Impl->BeforeTexture.Reset();
		Impl->CurrentTexture.Reset();
		Impl->bActive = false;
		return {.Status = bChanged ? EMaterialGraphCommandStatus::Succeeded
			: EMaterialGraphCommandStatus::NoChange};
	}

	auto FMaterialGraphParameterEditSession::Cancel()
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectCommand("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		Impl->bActive = false;
		if (Impl->BeforeValue.GetType() == EMaterialParameterType::Texture)
			Impl->BeforeValue.GetTexture().Texture = Impl->BeforeTexture.Get();
		if (!Material)
			return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		const bool bChanged = Impl->BeforeValue != Impl->CurrentValue;
		if (bChanged)
		{
			FScopedPackageDirtySuppression SuppressPackageRevision;
			if (const auto Restored = Material->SetParameterValue(
				Impl->ParameterId, Impl->BeforeValue); !Restored)
				return RejectCommand("The material rejected the original parameter value. " + FormatMaterialError(Restored.Error));
		}
		Impl->BeforeTexture.Reset();
		Impl->CurrentTexture.Reset();
		return {.Status = bChanged ? EMaterialGraphCommandStatus::Succeeded
			: EMaterialGraphCommandStatus::NoChange};
	}

	auto FMaterialGraphParameterEditSession::IsActive() const -> bool
	{
		return Impl->bActive;
	}
}
