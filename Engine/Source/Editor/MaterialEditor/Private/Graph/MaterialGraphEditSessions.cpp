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
		DTransactor* Transactions) -> std::expected<void, std::string>
	{
		if (Impl->bActive) return std::unexpected("A graph move is already active.");
		auto* Owner = Graph.GetMaterialOrFunction();
		if (!Owner) return std::unexpected("The material graph owner is no longer available.");
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return std::unexpected("The graph move selection is empty or exceeds the node bound.");
		if (Transactions && !Transactions->CanTransact()) return std::unexpected("The editor transactor is busy.");
		Impl->NodeIds.clear();
		for (const auto& Id : NodeIds)
		{
			if (!Graph.FindNode(Id))
				return std::unexpected("A selected graph node does not exist.");
			if (!Impl->NodeIds.insert(Id).second) return std::unexpected("The graph move selection contains a duplicate node GUID.");
		}
		Impl->Graph = &Graph;
		Impl->Draft.clear();
		Impl->Transactions = Transactions;
		Impl->AuthoredRevision = GraphSemanticRevision(*Owner);
		Impl->bActive = true;
		return {};
	}

	auto FMaterialGraphMoveSession::IsCurrent(const DObject& Owner) const -> bool
	{
		const auto* Graph = Impl->Graph.Get();
		return Impl->bActive && Graph && Graph->GetMaterialOrFunction() == &Owner
			&& GraphSemanticRevision(Owner) == Impl->AuthoredRevision;
	}

	auto FMaterialGraphMoveSession::Apply(std::span<const FMaterialGraphNodePresentation> Positions)
		-> std::expected<bool, std::string>
	{
		if (!Impl->bActive) return std::unexpected("No graph move is active.");
		const auto* Graph = Impl->Graph.Get();
		auto* Owner = Graph ? Graph->GetMaterialOrFunction() : nullptr;
		if (!Owner) { Cancel(); return std::unexpected("The material graph owner is no longer available."); }
		if (!IsCurrent(*Owner)) { Cancel(); return std::unexpected("The graph changed semantically during the move."); }
		if (Positions.size() > MaterialProgramMaxNodeCount) return std::unexpected("The graph move preview exceeds the node bound.");
		std::unordered_set<FGuid> Requested;
		for (const auto& Position : Positions)
		{
			if (!Impl->NodeIds.contains(Position.NodeId)) return std::unexpected("The graph move preview addresses a node outside the selection.");
			if (!Requested.insert(Position.NodeId).second) return std::unexpected("The graph move preview contains a duplicate node GUID.");
			if (Position.X < -MaterialGraphPresentationCoordinateLimit || Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit || Position.Y > MaterialGraphPresentationCoordinateLimit)
				return std::unexpected("The graph move preview is outside the supported coordinate range.");
		}
		bool bChanged = false;
		for (const auto& Position : Positions)
		{
			auto It = std::ranges::find(Impl->Draft, Position.NodeId, &FMaterialGraphNodePresentation::NodeId);
			if (It == Impl->Draft.end()) { Impl->Draft.push_back({Position.NodeId, Position.X, Position.Y}); bChanged = true; }
			else { bChanged |= It->X != Position.X || It->Y != Position.Y; It->X = Position.X; It->Y = Position.Y; }
		}
		return bChanged;
	}

	auto FMaterialGraphMoveSession::Commit() -> std::expected<void, std::string>
	{
		const auto Validated = Apply({});
		if (!Validated) return std::unexpected(Validated.error());
		std::string Error;
		if (!Impl->Graph.Get()->MoveNodesInternal(Impl->Draft, Impl->Transactions, &Error)) return std::unexpected(std::move(Error));
		Cancel();
		return {};
	}

	auto FMaterialGraphMoveSession::Cancel() -> void
	{
		Impl->bActive = false;
		Impl->Draft.clear();
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

		auto Finish() -> std::expected<void, std::string>
		{
			BeforeTexture.Reset();
			CurrentTexture.Reset();
			bActive = false;
			return {};
		}
	};

	FMaterialGraphParameterEditSession::FMaterialGraphParameterEditSession()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FMaterialGraphParameterEditSession::~FMaterialGraphParameterEditSession()
	{
		if (Impl && Impl->bActive) (void)Cancel();
	}

	auto FMaterialGraphParameterEditSession::Begin(
		DMaterial& Material,
		const FGuid& ParameterId,
		DTransactor* Transactions) -> std::expected<void, std::string>
	{
		if (Impl->bActive) return std::unexpected("A material parameter edit is already active.");
		if (!IsValid(&Material))
			return std::unexpected("The material graph owner is no longer available.");
		if (Transactions && !Transactions->CanTransact())
			return std::unexpected("The editor transactor is busy.");
		FResolvedMaterialParameter Resolved;
		if (!Material.ResolveParameterValue(ParameterId, Resolved)
			|| !Resolved.Definition)
			return std::unexpected("The material parameter definition is unavailable.");
		Impl->Material = &Material;
		Impl->ParameterId = ParameterId;
		Impl->BeforeValue = Resolved.Value;
		Impl->CurrentValue = Resolved.Value;
		Impl->BeforeTexture = Resolved.Value.GetType() == EMaterialParameterType::Texture
			? Resolved.Value.GetTexture().Texture.Get() : nullptr;
		Impl->CurrentTexture = Impl->BeforeTexture;
		Impl->Transactions = Transactions;
		Impl->bActive = true;
		return {};
	}

	auto FMaterialGraphParameterEditSession::Apply(FMaterialParameterValue Value)
		-> std::expected<bool, std::string>
	{
		if (!Impl->bActive) return std::unexpected("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return std::unexpected("The material graph owner is no longer available.");
		}
		if (Value == Impl->CurrentValue)
			return false;
		FScopedPackageDirtySuppression SuppressPackageRevision;
		if (const auto Applied = Material->SetParameterValue(Impl->ParameterId, Value); !Applied)
			return std::unexpected("The material rejected the parameter value. " + Applied.error());
		Impl->CurrentValue = std::move(Value);
		Impl->CurrentTexture = Impl->CurrentValue.GetType() == EMaterialParameterType::Texture
			? Impl->CurrentValue.GetTexture().Texture.Get() : nullptr;
		return true;
	}

	auto FMaterialGraphParameterEditSession::Commit()
		-> std::expected<void, std::string>
	{
		if (!Impl->bActive) return std::unexpected("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return std::unexpected("The material graph owner is no longer available.");
		}
		if (Impl->BeforeValue.GetType() == EMaterialParameterType::Texture)
		{
			Impl->BeforeValue.GetTexture().Texture = Impl->BeforeTexture.Get();
			Impl->CurrentValue.GetTexture().Texture = Impl->CurrentTexture.Get();
		}
		if (Impl->BeforeValue == Impl->CurrentValue) return Impl->Finish();
		if (Impl->Transactions && !Impl->Transactions->CanTransact())
			return std::unexpected("The editor transactor is busy.");
		{
			FScopedTransaction Recording(Impl->Transactions, {"MaterialGraph", "Edit material parameter"});
			if (Impl->Transactions)
			{
				const auto Recorded = Impl->Transactions->StoreUndo(
					MakeMaterialGraphParameterTransaction(
						*Material, Impl->ParameterId,
						Impl->BeforeValue, Impl->CurrentValue));
				if (!Recorded) return std::unexpected("Unable to record the graph edit. " + FormatTransactorResult(Recorded));
			}
			Material->MarkPackageDirty();
		}
		return Impl->Finish();
	}

	auto FMaterialGraphParameterEditSession::Cancel()
		-> std::expected<void, std::string>
	{
		if (!Impl->bActive) return std::unexpected("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		if (Impl->BeforeValue.GetType() == EMaterialParameterType::Texture)
			Impl->BeforeValue.GetTexture().Texture = Impl->BeforeTexture.Get();
		if (!Material)
			return std::unexpected("The material graph owner is no longer available.");
		if (Impl->BeforeValue == Impl->CurrentValue) return Impl->Finish();
		FScopedPackageDirtySuppression SuppressPackageRevision;
		if (const auto Restored = Material->SetParameterValue(Impl->ParameterId, Impl->BeforeValue); !Restored)
			return std::unexpected("The material rejected the original parameter value. " + Restored.error());
		return Impl->Finish();
	}

	auto FMaterialGraphParameterEditSession::IsActive() const -> bool
	{
		return Impl->bActive;
	}
}
