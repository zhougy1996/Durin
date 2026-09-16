#include "MaterialGraphEditInternals.h"
#include "MaterialGraphDocument.h"

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
		};

		// Replays only changed positions so unrelated presentation edits survive undo.
		class FMaterialGraphPresentationTransaction final
			: public ITransactionCustomChange
		{
		public:
			FMaterialGraphPresentationTransaction(
				DMaterial& InMaterial,
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
					else if (*After != Before)
						NodeChanges.push_back({Before.NodeId,
							FMaterialGraphPosition{Before.X, Before.Y},
							FMaterialGraphPosition{After->X, After->Y}});
				}
				for (const FMaterialGraphNodePresentation& After
					: AfterPresentation.Nodes)
				{
					if (std::ranges::none_of(BeforePresentation.Nodes,
						[&](const FMaterialGraphNodePresentation& Before) {
							return Before.NodeId == After.NodeId;
						}))
						NodeChanges.push_back({After.NodeId, std::nullopt,
							FMaterialGraphPosition{After.X, After.Y}});
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
				DMaterial* Target = Material.Get();
				if (!Target) return false;
				FMaterialGraphPresentation Candidate =
					Target->GetMaterialGraphPresentation();
				for (const FMaterialGraphNodePresentationChange& Change : NodeChanges)
				{
					const auto Current = std::ranges::find(
						Candidate.Nodes, Change.NodeId,
						&FMaterialGraphNodePresentation::NodeId);
					const auto& Desired = bBefore ? Change.Before : Change.After;
					if (!Desired)
					{
						if (Current != Candidate.Nodes.end()) Candidate.Nodes.erase(Current);
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
				return Target->SetMaterialGraphPresentation(std::move(Candidate));
			}

			TWeakObjectPtr<DMaterial> Material;
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
			DMaterial& Material,
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
			DMaterial& Material,
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

			const uint64 BeforeRevision =
				Material.GetMaterialGraphPresentationRevision();
			FMaterialGraphPresentation BeforePresentation;
			if (Transactions)
				BeforePresentation = Material.GetMaterialGraphPresentation();
			if (!Material.SetMaterialGraphPresentation(std::move(CandidatePresentation)))
				return MakeRejected("The material rejected the candidate graph presentation.");
			if (Material.GetMaterialGraphPresentationRevision() == BeforeRevision)
				return {.Status = EMaterialGraphCommandStatus::NoChange};

			if (Transactions)
			{
				const auto bRecorded = Transactions->CommitApplied(
					std::make_unique<FMaterialGraphPresentationTransaction>(
						Material,
						BeforePresentation,
						Material.GetMaterialGraphPresentation(),
						std::move(Description)));
				check(bRecorded);
			}
			std::ranges::sort(Affected);
			Affected.erase(std::unique(Affected.begin(), Affected.end()), Affected.end());
			return {
				.Status = EMaterialGraphCommandStatus::Succeeded,
				.AffectedNodeIds = std::move(Affected),
			};
		}
	}
}
