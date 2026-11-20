#include "MaterialGraphEditInternals.h"
#include "Graph/MaterialGraphInteraction.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/Material.h"
#include "MaterialGraph.h"

#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	namespace
	{
		auto GraphSemanticRevision(const DObject& Owner) -> uint64
		{
			if (const auto* Material = Cast<DMaterial>(&Owner)) return Material->GetMaterialContentRevision();
			return Cast<DMaterialFunction>(&Owner)->GetFunctionRevision();
		}
	}

	struct FMaterialGraphMoveSession::FImpl
	{
		TWeakObjectPtr<DMaterialGraph> Graph;
		std::unordered_set<FGuid> NodeIds;
		std::vector<FMaterialGraphNodePresentation> Draft;
		DTransactor* Transactions = nullptr;
		uint64 AuthoredRevision = 0;
		bool bActive = false;
	};

	FMaterialGraphMoveSession::FMaterialGraphMoveSession() : Impl(std::make_unique<FImpl>()) {}
	FMaterialGraphMoveSession::~FMaterialGraphMoveSession() = default;

	auto FMaterialGraphMoveSession::Begin(DMaterialGraph& Graph, std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Impl->bActive) return RejectCommand("A graph move is already active.");
		auto* Owner = Graph.GetMaterialOrFunction();
		if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return RejectCommand("The graph move selection is empty or exceeds the node bound.");
		if (Transactions && !Transactions->CanTransact()) return RejectCommand("The editor transactor is busy.");
		Impl->NodeIds.clear();
		for (const auto& Id : NodeIds)
		{
			if (!Graph.FindNode(Id))
				return RejectCommand("A selected graph node does not exist.");
			if (!Impl->NodeIds.insert(Id).second) return RejectCommand("The graph move selection contains a duplicate node GUID.");
		}
		Impl->Graph = &Graph;
		Impl->Draft.clear();
		Impl->Transactions = Transactions;
		Impl->AuthoredRevision = GraphSemanticRevision(*Owner);
		Impl->bActive = true;
		return {.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::vector<FGuid>(NodeIds.begin(), NodeIds.end())};
	}

	auto FMaterialGraphMoveSession::IsCurrent(const DObject& Owner) const -> bool
	{
		const auto* Graph = Impl->Graph.Get();
		return Impl->bActive && Graph && Graph->GetMaterialOrFunction() == &Owner
			&& GraphSemanticRevision(Owner) == Impl->AuthoredRevision;
	}

	auto FMaterialGraphMoveSession::Apply(std::span<const FMaterialGraphNodePresentation> Positions)
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectCommand("No graph move is active.");
		const auto* Graph = Impl->Graph.Get();
		auto* Owner = Graph ? Graph->GetMaterialOrFunction() : nullptr;
		if (!Owner) { Cancel(); return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner); }
		if (!IsCurrent(*Owner)) { Cancel(); return RejectCommand("The graph changed semantically during the move."); }
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
		auto Result = Impl->Graph.Get()->MoveNodesInternal(Impl->Draft, Impl->Transactions);
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
		TStrongObjectPtr<DTexture> BeforeTexture;
		TStrongObjectPtr<DTexture> CurrentTexture;
		DTransactor* Transactions = nullptr;
		bool bActive = false;

		auto Finish(EMaterialGraphCommandStatus Status) -> FMaterialGraphCommandResult
		{
			BeforeTexture.Reset();
			CurrentTexture.Reset();
			bActive = false;
			return {.Status = Status};
		}
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
		if (Transactions && !Transactions->CanTransact())
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
			return RejectCommand("The material rejected the parameter value. " + Applied.error());
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
		if (Impl->BeforeValue.GetType() == EMaterialParameterType::Texture)
		{
			Impl->BeforeValue.GetTexture().Texture = Impl->BeforeTexture.Get();
			Impl->CurrentValue.GetTexture().Texture = Impl->CurrentTexture.Get();
		}
		if (Impl->BeforeValue == Impl->CurrentValue) return Impl->Finish(EMaterialGraphCommandStatus::NoChange);
		if (Impl->Transactions && !Impl->Transactions->CanTransact())
			return RejectCommand("The editor transactor is busy.");
		FScopedTransaction Recording(Impl->Transactions, {"MaterialGraph", "Edit material parameter"});
		if (Impl->Transactions)
		{
			const auto Recorded = Impl->Transactions->StoreUndo(Material,
				MakeMaterialGraphParameterTransaction(
					*Material, Impl->ParameterId,
					Impl->BeforeValue, Impl->CurrentValue));
			if (!Recorded) return RejectCommand("Unable to record the graph edit. " + FormatTransactorResult(Recorded));
		}
		Material->MarkPackageDirty();
		(void)Recording.End();
		if (Impl->Transactions && Impl->Transactions->GetLastResult().Code == ETransactorResultCode::Failed)
			return RejectCommand(FormatTransactorResult(Impl->Transactions->GetLastResult()));
		return Impl->Finish(EMaterialGraphCommandStatus::Succeeded);
	}

	auto FMaterialGraphParameterEditSession::Cancel()
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return RejectCommand("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		if (Impl->BeforeValue.GetType() == EMaterialParameterType::Texture)
			Impl->BeforeValue.GetTexture().Texture = Impl->BeforeTexture.Get();
		if (!Material)
			return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (Impl->BeforeValue == Impl->CurrentValue) return Impl->Finish(EMaterialGraphCommandStatus::NoChange);
		FScopedPackageDirtySuppression SuppressPackageRevision;
		if (const auto Restored = Material->SetParameterValue(Impl->ParameterId, Impl->BeforeValue); !Restored)
			return RejectCommand("The material rejected the original parameter value. " + Restored.error());
		return Impl->Finish(EMaterialGraphCommandStatus::Succeeded);
	}

	auto FMaterialGraphParameterEditSession::IsActive() const -> bool
	{
		return Impl->bActive;
	}
}
