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
		struct FMaterialGraphPosition
		{
			int32 X = 0;
			int32 Y = 0;
		};

		struct FMaterialGraphNodePresentationChange
		{
			FGuid NodeId;
			std::optional<FMaterialGraphPosition> Before;
			std::optional<FMaterialGraphPosition> After;
			FMaterialGraphPosition UnauthoredPosition;
		};

		// Replays only changed positions so unrelated presentation edits survive undo.
		class FMaterialGraphPresentationTransaction final
			: public ITransactionCustomChange
		{
		public:
			FMaterialGraphPresentationTransaction(
				DObject& InMaterial,
				const FMaterialGraphPresentation& BeforePresentation,
				const FMaterialGraphPresentation& AfterPresentation,
				std::string InDescription)
				: Material(&InMaterial)
				, Description(std::move(InDescription))
			{
				for (const FMaterialGraphNodePresentation& Before
					: BeforePresentation.Nodes)
				{
					const auto After = std::ranges::find(
						AfterPresentation.Nodes, Before.NodeId,
						&FMaterialGraphNodePresentation::NodeId);
					if (After == AfterPresentation.Nodes.end())
						NodeChanges.push_back({Before.NodeId,
							FMaterialGraphPosition{Before.X, Before.Y}, std::nullopt});
					else if (After->X != Before.X || After->Y != Before.Y)
						NodeChanges.push_back({Before.NodeId,
							FMaterialGraphPosition{Before.X, Before.Y},
							FMaterialGraphPosition{After->X, After->Y}});
				}
				std::optional<FMaterialGraphView> BeforeView;
				for (const FMaterialGraphNodePresentation& After
					: AfterPresentation.Nodes)
				{
					if (std::ranges::none_of(BeforePresentation.Nodes,
						[&](const FMaterialGraphNodePresentation& Before) {
							return Before.NodeId == After.NodeId;
						}))
					{
						if (!BeforeView) BeforeView = FMaterialGraphDocument(InMaterial).Inspect();
						const auto Node = std::ranges::find(BeforeView->Nodes, After.NodeId,
							[](const auto& View) { return View.Node.Id; });
						const auto Fallback = Node == BeforeView->Nodes.end() ? FMaterialGraphPosition{}
							: FMaterialGraphPosition{Node->Presentation.X, Node->Presentation.Y};
						NodeChanges.push_back({After.NodeId, std::nullopt,
							FMaterialGraphPosition{After.X, After.Y}, Fallback});
					}
				}

				AffectedPackages.front() = InMaterial.GetPackage();
			}

			auto GetDescription() const -> std::string_view override
			{
				return Description;
			}
			auto GetOwningModule() const -> std::string_view override
			{
				return "MaterialEditor";
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override
			{
				return AffectedPackages;
			}
			auto Undo() -> bool override { return Apply(true); }
			auto Redo() -> bool override { return Apply(false); }
			auto GetAllocatedSize() const -> size_t override
			{
				return Description.capacity()
					+ NodeChanges.capacity()
						* sizeof(FMaterialGraphNodePresentationChange);
			}

		private:
			auto Apply(bool bBefore) -> bool
			{
				DObject* Target = Material.Get();
				if (!Target) return false;
				FMaterialGraphPresentation Candidate =
					ReadGraphPresentation(*Target);
				for (const FMaterialGraphNodePresentationChange& Change : NodeChanges)
				{
					const auto Current = std::ranges::find(
						Candidate.Nodes, Change.NodeId,
						&FMaterialGraphNodePresentation::NodeId);
					const auto& Desired = bBefore ? Change.Before : Change.After;
					if (!Desired)
					{
						// A label authored after the move owns this entry independently.
						if (Current != Candidate.Nodes.end())
						{
							if (Current->DisplayName.empty()) Candidate.Nodes.erase(Current);
							else { Current->X = Change.UnauthoredPosition.X; Current->Y = Change.UnauthoredPosition.Y; }
						}
					}
					else if (Current == Candidate.Nodes.end())
						Candidate.Nodes.push_back(
							{Change.NodeId, Desired->X, Desired->Y});
					else
					{
						Current->X = Desired->X;
						Current->Y = Desired->Y;
					}
				}
				return WriteGraphPresentation(*Target, std::move(Candidate))
					!= EMaterialGraphPresentationResult::Rejected;
			}

			TWeakObjectPtr<DObject> Material;
			std::vector<FMaterialGraphNodePresentationChange> NodeChanges;
			std::string Description;
			std::array<DPackage*, 1> AffectedPackages{};
		};

		// Retains both parameter values and their texture references for undo/redo.
		class FMaterialGraphParameterTransaction final : public ITransactionCustomChange
		{
		public:
			FMaterialGraphParameterTransaction(
				DMaterial& InMaterial,
				FGuid InParameterId,
				FMaterialParameterValue InBeforeValue,
				FMaterialParameterValue InAfterValue)
				: Material(&InMaterial)
				, ParameterId(InParameterId)
				, BeforeValue(std::move(InBeforeValue))
				, AfterValue(std::move(InAfterValue))
			{
				AffectedPackages.front() = InMaterial.GetPackage();
			}

			auto GetDescription() const -> std::string_view override
			{
				return "Edit Material Parameter";
			}
			auto GetOwningModule() const -> std::string_view override
			{
				return "MaterialEditor";
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override
			{
				return AffectedPackages;
			}
			auto Undo() -> bool override { return Apply(BeforeValue); }
			auto Redo() -> bool override { return Apply(AfterValue); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				BeforeValue.AddReferencedObjects(Collector);
				AfterValue.AddReferencedObjects(Collector);
			}

		private:
			auto Apply(const FMaterialParameterValue& Value) -> bool
			{
				DMaterial* Target = Material.Get();
				return Target && Target->SetParameterValue(ParameterId, Value);
			}

			TWeakObjectPtr<DMaterial> Material;
			FGuid ParameterId;
			mutable FMaterialParameterValue BeforeValue;
			mutable FMaterialParameterValue AfterValue;
			std::array<DPackage*, 1> AffectedPackages{};
		};

	}

	namespace GraphEditInternals
	{
		auto MakeMaterialGraphPresentationTransaction(
			DObject& Material,
			const FMaterialGraphPresentation& BeforePresentation,
			const FMaterialGraphPresentation& AfterPresentation,
			std::string Description)
			-> std::unique_ptr<ITransactionCustomChange>
		{
			return std::make_unique<FMaterialGraphPresentationTransaction>(
				Material, BeforePresentation, AfterPresentation, std::move(Description));
		}

		auto MakeMaterialGraphParameterTransaction(
			DMaterial& Material,
			FGuid ParameterId,
			FMaterialParameterValue BeforeValue,
			FMaterialParameterValue AfterValue)
			-> std::unique_ptr<ITransactionCustomChange>
		{
			return std::make_unique<FMaterialGraphParameterTransaction>(
				Material, ParameterId, std::move(BeforeValue), std::move(AfterValue));
		}

		auto CommitPresentationChange(
			DObject& Material,
			FMaterialGraphPresentation CandidatePresentation,
			std::string Description,
			std::vector<FGuid> Affected,
			DTransactor* Transactions) -> FMaterialGraphCommandResult
		{
			if (!IsValid(&Material))
				return {.Status = EMaterialGraphCommandStatus::StaleOwner,
					.Message = "The material graph owner is no longer available."};
			if (Transactions && Transactions->HasPendingOperation())
				return MakeRejected("The editor transactor is busy.");

			std::vector<FGuid> Ids;
			for (const auto& Expression : FMaterialExpressionEditing::GetExpressions(Material)) Ids.push_back(Expression->Id);
			CandidatePresentation = SanitizeMaterialGraphPresentation(CandidatePresentation, Ids);
			const auto BeforePresentation = ReadGraphPresentation(Material);
			if (BeforePresentation == CandidatePresentation) return {.Status = EMaterialGraphCommandStatus::NoChange};
			if (Transactions && !Transactions->CommitApplied(
				MakeMaterialGraphPresentationTransaction(Material, BeforePresentation, CandidatePresentation, std::move(Description))))
				return MakeRejected("Unable to record the graph move.");
			// The validated candidate is published only after history accepts the edit.
			const auto Result = WriteGraphPresentation(Material, std::move(CandidatePresentation));
			check(Result != EMaterialGraphPresentationResult::Rejected);
			std::ranges::sort(Affected);
			Affected.erase(std::unique(Affected.begin(), Affected.end()), Affected.end());
			return {
				.Status = EMaterialGraphCommandStatus::Succeeded,
				.AffectedNodeIds = std::move(Affected),
			};
		}
	}
}
