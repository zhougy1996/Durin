#include "MaterialGraphEditInternals.h"

#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	struct FMaterialGraphMoveSession::FImpl
	{
		TWeakObjectPtr<DMaterial> Material;
		FMaterialGraphPresentation BeforePresentation;
		std::unordered_set<FGuid> NodeIds;
		DTransactor* Transactions = nullptr;
		uint64 AuthoredRevision = 0;
		bool bMaterialOutput = false;
		bool bActive = false;

		auto AbortStaleSemanticChange(DMaterial& Target)
			-> FMaterialGraphCommandResult
		{
			FMaterialGraphPresentation Restored =
				Target.GetMaterialGraphPresentation();
			if (bMaterialOutput)
			{
				Restored.bHasMaterialOutputPosition =
					BeforePresentation.bHasMaterialOutputPosition;
				Restored.MaterialOutputX = BeforePresentation.MaterialOutputX;
				Restored.MaterialOutputY = BeforePresentation.MaterialOutputY;
			}
			else
			{
				for (const FGuid& NodeId : NodeIds)
				{
					auto Current = std::ranges::find(
						Restored.Nodes, NodeId,
						&FMaterialGraphNodePresentation::NodeId);
					const auto Before = std::ranges::find(
						BeforePresentation.Nodes, NodeId,
						&FMaterialGraphNodePresentation::NodeId);
					if (Before == BeforePresentation.Nodes.end())
					{
						if (Current != Restored.Nodes.end())
							Restored.Nodes.erase(Current);
					}
					else if (Current == Restored.Nodes.end())
						Restored.Nodes.push_back(*Before);
					else
						*Current = *Before;
				}
			}
			Target.SetMaterialGraphPresentation(std::move(Restored));
			bActive = false;
			return MakeRejected(
				"The material changed semantically during the graph move.");
		}
	};

	FMaterialGraphMoveSession::FMaterialGraphMoveSession()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FMaterialGraphMoveSession::~FMaterialGraphMoveSession()
	{
		if (Impl && Impl->bActive) Cancel();
	}

	auto FMaterialGraphMoveSession::Begin(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Impl->bActive) return MakeRejected("A material graph move is already active.");
		if (!IsValid(&Material))
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph move selection is empty or exceeds the node bound.");
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		Impl->NodeIds.clear();
		for (const FGuid& Id : NodeIds)
		{
			if (!FindNode(*Material.GetMaterialProgram(), Id))
				return MakeRejected("A selected material graph node does not exist.");
			if (!Impl->NodeIds.insert(Id).second)
				return MakeRejected("The material graph move selection contains a duplicate node GUID.");
		}
		Impl->Material = &Material;
		Impl->BeforePresentation = Material.GetMaterialGraphPresentation();
		Impl->Transactions = Transactions;
		Impl->AuthoredRevision =
			Material.GetMaterialCompileStatus().AuthoredRevision;
		Impl->bMaterialOutput = false;
		Impl->bActive = true;
		std::vector<FGuid> Affected(NodeIds.begin(), NodeIds.end());
		return {.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(Affected)};
	}

	auto FMaterialGraphMoveSession::BeginMaterialOutput(
		DMaterial& Material,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Impl->bActive) return MakeRejected("A material graph move is already active.");
		if (!IsValid(&Material))
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		Impl->Material = &Material;
		Impl->BeforePresentation = Material.GetMaterialGraphPresentation();
		Impl->NodeIds.clear();
		Impl->Transactions = Transactions;
		Impl->AuthoredRevision =
			Material.GetMaterialCompileStatus().AuthoredRevision;
		Impl->bMaterialOutput = true;
		Impl->bActive = true;
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}

	auto FMaterialGraphMoveSession::Apply(
		std::span<const FMaterialGraphNodePresentation> Positions)
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return MakeRejected("No material graph move is active.");
		if (Impl->bMaterialOutput)
			return MakeRejected("The active move addresses Material Output, not graph nodes.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		}
		if (Material->GetMaterialCompileStatus().AuthoredRevision
			!= Impl->AuthoredRevision)
			return Impl->AbortStaleSemanticChange(*Material);
		if (Positions.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Positions.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph move preview exceeds the node bound.");
		std::unordered_set<FGuid> RequestedNodes;
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			if (!Impl->NodeIds.contains(Position.NodeId))
				return MakeRejected("A material graph move preview addresses a node outside the selection.");
			if (!RequestedNodes.insert(Position.NodeId).second)
				return MakeRejected("A material graph move preview contains a duplicate node GUID.");
			if (Position.X < -MaterialGraphPresentationCoordinateLimit
				|| Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit
				|| Position.Y > MaterialGraphPresentationCoordinateLimit)
				return MakeRejected("A material graph move preview is outside the supported coordinate range.");
		}
		const uint64 BeforeRevision =
			Material->GetMaterialGraphPresentationRevision();
		if (!Material->ApplyMaterialGraphNodePositions(
			Positions, Impl->AuthoredRevision))
			return MakeRejected("The material rejected the graph move preview.");
		return {.Status = Material->GetMaterialGraphPresentationRevision()
			== BeforeRevision ? EMaterialGraphCommandStatus::NoChange
			: EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::vector<FGuid>(
				RequestedNodes.begin(), RequestedNodes.end())};
	}

	auto FMaterialGraphMoveSession::ApplyMaterialOutput(int32 X, int32 Y)
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return MakeRejected("No material graph move is active.");
		if (!Impl->bMaterialOutput)
			return MakeRejected("The active move addresses graph nodes, not Material Output.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		}
		if (Material->GetMaterialCompileStatus().AuthoredRevision
			!= Impl->AuthoredRevision)
			return Impl->AbortStaleSemanticChange(*Material);
		if (X < -MaterialGraphPresentationCoordinateLimit
			|| X > MaterialGraphPresentationCoordinateLimit
			|| Y < -MaterialGraphPresentationCoordinateLimit
			|| Y > MaterialGraphPresentationCoordinateLimit)
			return MakeRejected(
				"The Material Output move preview is outside the supported coordinate range.");
		const uint64 BeforeRevision =
			Material->GetMaterialGraphPresentationRevision();
		if (!Material->ApplyMaterialGraphOutputPosition(
			X, Y, Impl->AuthoredRevision))
			return MakeRejected("The material rejected the Material Output move preview.");
		return {.Status = Material->GetMaterialGraphPresentationRevision()
			== BeforeRevision ? EMaterialGraphCommandStatus::NoChange
			: EMaterialGraphCommandStatus::Succeeded};
	}

	auto FMaterialGraphMoveSession::Commit() -> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return MakeRejected("No material graph move is active.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		}
		if (Impl->Transactions && Impl->Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		if (Material->GetMaterialCompileStatus().AuthoredRevision
			!= Impl->AuthoredRevision)
			return Impl->AbortStaleSemanticChange(*Material);
		const FMaterialGraphPresentation& CurrentPresentation =
			Material->GetMaterialGraphPresentation();
		const bool bChanged = Impl->BeforePresentation != CurrentPresentation;
		if (bChanged && Impl->Transactions)
		{
			const auto bRecorded = Impl->Transactions->CommitApplied(
				MakeMaterialGraphPresentationTransaction(
					*Material,
					Impl->BeforePresentation,
					CurrentPresentation,
					Impl->bMaterialOutput
						? "Move Material Output" : "Move Material Nodes"));
			check(bRecorded);
		}
		std::vector<FGuid> Affected(Impl->NodeIds.begin(), Impl->NodeIds.end());
		Impl->bActive = false;
		return {
			.Status = bChanged ? EMaterialGraphCommandStatus::Succeeded
				: EMaterialGraphCommandStatus::NoChange,
			.AffectedNodeIds = std::move(Affected),
		};
	}

	auto FMaterialGraphMoveSession::Cancel() -> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return MakeRejected("No material graph move is active.");
		DMaterial* Material = Impl->Material.Get();
		Impl->bActive = false;
		if (!Material)
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		const bool bChanged = Impl->BeforePresentation
			!= Material->GetMaterialGraphPresentation();
		Material->SetMaterialGraphPresentation(Impl->BeforePresentation);
		return {.Status = bChanged ? EMaterialGraphCommandStatus::Succeeded
			: EMaterialGraphCommandStatus::NoChange};
	}

	auto FMaterialGraphMoveSession::IsActive() const -> bool
	{
		return Impl->bActive;
	}

	struct FMaterialGraphParameterEditSession::FImpl
	{
		TWeakObjectPtr<DMaterial> Material;
		FGuid ParameterId;
		FMaterialParameterValue BeforeValue;
		FMaterialParameterValue CurrentValue;
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
		if (Impl->bActive) return MakeRejected("A material parameter edit is already active.");
		if (!IsValid(&Material))
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		const std::vector Dependencies = InspectMaterialParameterDependencies(
			*Material.GetMaterialProgram(), Material.GetParameterDefinitions(), Material.GetMaterialFunctionCalls());
		if (std::ranges::none_of(Dependencies, [&](const auto& Dependency) {
			return Dependency.ParameterId == ParameterId;
		}))
			return MakeRejected("Only a reachable material graph parameter can be edited here.");
		FResolvedMaterialParameter Resolved;
		if (!Material.ResolveParameterValue(ParameterId, Resolved)
			|| !Resolved.Definition)
			return MakeRejected("The material parameter definition is unavailable.");
		Impl->Material = &Material;
		Impl->ParameterId = ParameterId;
		Impl->BeforeValue = Resolved.Value;
		Impl->CurrentValue = Resolved.Value;
		Impl->Transactions = Transactions;
		Impl->bActive = true;
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}

	auto FMaterialGraphParameterEditSession::Apply(FMaterialParameterValue Value)
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return MakeRejected("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		}
		if (Value == Impl->CurrentValue)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		FScopedPackageDirtySuppression SuppressPackageRevision;
		if (!Material->SetParameterValue(Impl->ParameterId, Value))
			return MakeRejected("The material rejected the parameter value.");
		Impl->CurrentValue = std::move(Value);
		std::vector<FGuid> AffectedNodes;
		for (const FMaterialProgramNode& Node : Material->GetMaterialProgram()->Nodes)
			if (Node.ParameterId == Impl->ParameterId) AffectedNodes.push_back(Node.Id);
		return {.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(AffectedNodes)};
	}

	auto FMaterialGraphParameterEditSession::Commit()
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return MakeRejected("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		}
		if (Impl->Transactions && Impl->Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		const bool bChanged = Impl->BeforeValue != Impl->CurrentValue;
		if (bChanged && Impl->Transactions)
		{
			const auto bRecorded = Impl->Transactions->CommitApplied(
				MakeMaterialGraphParameterTransaction(
					*Material, Impl->ParameterId,
					Impl->BeforeValue, Impl->CurrentValue));
			check(bRecorded);
		}
		if (bChanged) Material->MarkPackageDirty();
		Impl->bActive = false;
		return {.Status = bChanged ? EMaterialGraphCommandStatus::Succeeded
			: EMaterialGraphCommandStatus::NoChange};
	}

	auto FMaterialGraphParameterEditSession::Cancel()
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return MakeRejected("No material parameter edit is active.");
		DMaterial* Material = Impl->Material.Get();
		Impl->bActive = false;
		if (!Material)
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		const bool bChanged = Impl->BeforeValue != Impl->CurrentValue;
		if (bChanged)
		{
			FScopedPackageDirtySuppression SuppressPackageRevision;
			if (!Material->SetParameterValue(
				Impl->ParameterId, Impl->BeforeValue))
				return MakeRejected("The material rejected the original parameter value.");
		}
		return {.Status = bChanged ? EMaterialGraphCommandStatus::Succeeded
			: EMaterialGraphCommandStatus::NoChange};
	}

	auto FMaterialGraphParameterEditSession::IsActive() const -> bool
	{
		return Impl->bActive;
	}
}
