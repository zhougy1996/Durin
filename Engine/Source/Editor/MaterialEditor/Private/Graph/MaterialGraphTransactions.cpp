#include "MaterialGraph.h"
#include "MaterialGraphEditInternals.h"
#include "Graph/MaterialGraphChange.h"

#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	namespace
	{
		// Replays only changed positions so unrelated presentation edits survive undo.
		class FMaterialGraphPresentationTransaction final
			: public FMaterialCommandChange
		{
		public:
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{ return {Material.Get()}; }

			FMaterialGraphPresentationTransaction(
				DObject& Owner, std::vector<FNodePositionChange> Changes, std::string InDescription)
				: Material(&Owner)
				, NodeChanges(std::move(Changes))
				, Description(std::move(InDescription))
				, TargetPath(Owner.GetObjectPath())
			{
				AffectedPackages.front() = Material.Get()->GetPackage();
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
			auto Replay(ETransactionOperation Operation) -> FMaterialChangeResult override
			{ return Apply(Operation == ETransactionOperation::Undo); }
			auto GetAllocatedSize() const -> size_t override
			{
				return Description.capacity() + TargetPath.capacity()
					+ NodeChanges.capacity()
						* sizeof(FNodePositionChange);
			}

		private:
			auto Apply(bool bBefore) -> FMaterialChangeResult
			{
				DObject* Target = Material.Get();
				if (!Target) return {{.Code = EMaterialChangeError::TargetUnavailable, .TargetPath = TargetPath, .NodeCount = NodeChanges.size()}};
				FMaterialGraphPresentation Candidate =
					ReadGraphPresentation(*Target);
				std::unordered_map<FGuid, size_t> Indices;
				for (size_t I = 0; I < Candidate.Nodes.size(); ++I) Indices.emplace(Candidate.Nodes[I].NodeId, I);
				std::unordered_set<FGuid> Remove;
				for (const auto& Change : NodeChanges)
				{
					const auto It = Indices.find(Change.NodeId);
					const auto X = bBefore ? Change.BeforeX : Change.AfterX;
					const auto Y = bBefore ? Change.BeforeY : Change.AfterY;
					if (It == Indices.end())
					{
						if (!bBefore || Change.HadAuthoredPosition)
							Candidate.Nodes.push_back({Change.NodeId, X, Y});
						continue;
					}
					auto& Current = Candidate.Nodes[It->second];
					// Keep a later label at the original default coordinates.
					if (bBefore && !Change.HadAuthoredPosition && Current.DisplayName.empty())
						Remove.insert(Change.NodeId);
					else { Current.X = X; Current.Y = Y; }
				}
				if (!Remove.empty()) std::erase_if(Candidate.Nodes, [&](const auto& Node) { return Remove.contains(Node.NodeId); });
				const auto Count = Candidate.Nodes.size();
				if (WriteGraphPresentation(*Target, std::move(Candidate)) == EMaterialGraphPresentationResult::Rejected)
					return {{.Code = EMaterialChangeError::PresentationWrite, .TargetPath = TargetPath, .NodeCount = Count}};
				return {};
			}

			TWeakObjectPtr<DObject> Material;
			std::vector<FNodePositionChange> NodeChanges;
			std::string Description;
			std::string TargetPath;
			std::array<DPackage*, 1> AffectedPackages{};
		};

		// Retains both parameter values and their texture references for undo/redo.
		class FMaterialGraphParameterTransaction final : public FMaterialCommandChange
		{
		public:
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{ return {Material.Get()}; }

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
			auto Replay(ETransactionOperation Operation) -> FMaterialChangeResult override
			{ return Apply(Operation == ETransactionOperation::Undo ? BeforeValue : AfterValue); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				BeforeValue.AddReferencedObjects(Collector);
				AfterValue.AddReferencedObjects(Collector);
			}

		private:
			auto Apply(const FMaterialParameterValue& Value) -> FMaterialChangeResult
			{
				DMaterial* Target = Material.Get();
				if (!Target) return {{.Code = EMaterialChangeError::TargetUnavailable, .ParameterId = ParameterId}};
				const auto Applied = Target->SetParameterValue(ParameterId, Value);
				if (!Applied) return {{.Code = EMaterialChangeError::MaterialWrite, .ParameterId = ParameterId,
					.Message = Applied.error()}};
				return {};
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
			DObject& Owner, std::vector<FNodePositionChange> Changes, std::string Description)
			-> std::unique_ptr<FMaterialCommandChange>
		{
			return std::make_unique<FMaterialGraphPresentationTransaction>(
				Owner, std::move(Changes), std::move(Description));
		}

		auto MakeMaterialGraphParameterTransaction(
			DMaterial& Material,
			FGuid ParameterId,
			FMaterialParameterValue BeforeValue,
			FMaterialParameterValue AfterValue)
			-> std::unique_ptr<FMaterialCommandChange>
		{
			return std::make_unique<FMaterialGraphParameterTransaction>(
				Material, ParameterId, std::move(BeforeValue), std::move(AfterValue));
		}

		auto CommitPresentationChange(
			DObject& Owner,
			FMaterialGraphPresentation CandidatePresentation,
			std::string Description,
			std::vector<FNodePositionChange> Changes,
			DTransactor* Transactions, std::string* OutError) -> bool
		{
			if (Transactions && !Transactions->CanTransact())
				return FailWithError(OutError, "The editor transactor is busy.");

			if (Changes.empty()) return true;
			FScopedTransaction Recording(Transactions, {"MaterialGraph", Description});
			if (Transactions)
			{
				const auto Recorded = Transactions->StoreUndo(
					MakeMaterialGraphPresentationTransaction(Owner, std::move(Changes), std::move(Description)));
				if (!Recorded)
				{
					if (OutError) *OutError = "Unable to record the graph edit. " + FormatTransactorResult(Recorded);
					return false;
				}
			}
			// The validated candidate is published only after history accepts the edit.
			const auto Result = WriteGraphPresentation(Owner, std::move(CandidatePresentation));
			check(Result != EMaterialGraphPresentationResult::Rejected);
			return true;
		}
	}
}
