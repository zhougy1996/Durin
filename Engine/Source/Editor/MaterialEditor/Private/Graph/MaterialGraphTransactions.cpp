#include "MaterialGraphEditInternals.h"

#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin::Editor::Material
{
	using namespace GraphEditInternals;

	namespace
	{
		auto GetMaterialProgramAllocatedSize(const FMaterialProgram& Program) -> size_t
		{
			size_t Size = Program.Nodes.capacity() * sizeof(FMaterialProgramNode);
			for (const FMaterialProgramNode& Node : Program.Nodes)
			{
				Size += Node.Inputs.capacity() * sizeof(FMaterialProgramLink);
				Size += Node.DisplayName.capacity();
			}
			return Size;
		}

		auto GetMaterialGraphPresentationAllocatedSize(
			const FMaterialGraphPresentation& Presentation) -> size_t
		{
			return Presentation.Nodes.capacity()
				* sizeof(FMaterialGraphNodePresentation);
		}

		// Restores the program, presentation, and optional promoted parameter together.
		class FMaterialGraphSemanticTransaction final : public ITransactionCustomChange
		{
		public:
			FMaterialGraphSemanticTransaction(
				DMaterial& InMaterial,
				FMaterialProgram InBeforeProgram,
				FMaterialGraphPresentation InBeforePresentation,
				FMaterialProgram InAfterProgram,
				FMaterialGraphPresentation InAfterPresentation,
				std::string InDescription,
				FGuid InParameterId = {},
				FMaterialParameterValue InBeforeParameterValue = {},
				FMaterialParameterValue InAfterParameterValue = {})
				: Material(&InMaterial)
				, BeforeProgram(std::move(InBeforeProgram))
				, BeforePresentation(std::move(InBeforePresentation))
				, AfterProgram(std::move(InAfterProgram))
				, AfterPresentation(std::move(InAfterPresentation))
				, Description(std::move(InDescription))
				, ParameterId(InParameterId)
				, BeforeParameterValue(std::move(InBeforeParameterValue))
				, AfterParameterValue(std::move(InAfterParameterValue))
			{
				AffectedPackages.front() = InMaterial.GetPackage();
			}

			auto GetDescription() const -> std::string_view override
			{
				return Description;
			}
			auto GetOwningModule() const -> std::string_view override { return "MaterialEditor"; }

			auto GetAffectedPackages() const -> std::span<DPackage* const> override
			{
				return AffectedPackages;
			}

			auto Undo() -> bool override
			{
				return Apply(BeforeProgram, BeforePresentation,
					BeforeParameterValue);
			}

			auto Redo() -> bool override
			{
				return Apply(AfterProgram, AfterPresentation,
					AfterParameterValue);
			}
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (DObject* Object : {
					static_cast<DObject*>(BeforeParameterValue.TextureValue.Get()),
					static_cast<DObject*>(AfterParameterValue.TextureValue.Get())})
					if (Object) Collector.AddReferencedObject(Object);
			}
			auto GetAllocatedSize() const -> size_t override
			{
				return Description.capacity()
					+ GetMaterialProgramAllocatedSize(BeforeProgram)
					+ GetMaterialGraphPresentationAllocatedSize(BeforePresentation)
					+ GetMaterialProgramAllocatedSize(AfterProgram)
					+ GetMaterialGraphPresentationAllocatedSize(AfterPresentation);
			}

		private:
				auto Apply(
				const FMaterialProgram& Program,
				const FMaterialGraphPresentation& Presentation,
				const FMaterialParameterValue& ParameterValue) -> bool
			{
				DMaterial* Target = Material.Get();
				if (!Target) return false;
				if (ParameterId.IsValid()
					&& !Target->SetParameterValue(ParameterId, ParameterValue))
					return false;
				FMaterialProgramValidationResult Validation;
				if (!(Validation = Target->SetMaterialProgram(Program))) return false;
				return Target->SetMaterialGraphPresentation(Presentation);
			}

			TWeakObjectPtr<DMaterial> Material;
			FMaterialProgram BeforeProgram;
			FMaterialGraphPresentation BeforePresentation;
			FMaterialProgram AfterProgram;
			FMaterialGraphPresentation AfterPresentation;
			std::string Description;
			FGuid ParameterId;
			FMaterialParameterValue BeforeParameterValue;
			FMaterialParameterValue AfterParameterValue;
			std::array<DPackage*, 1> AffectedPackages{};
		};

		// Retains declaration texture defaults across Undo/Redo through the collector.
		class FMaterialDeclarationTransaction final : public ITransactionCustomChange
		{
		public:
			using FState = FMaterialDeclarationState;

			FMaterialDeclarationTransaction(DMaterial& Owner, FState InBefore, FState InAfter)
				: Material(&Owner), Before(std::move(InBefore)), After(std::move(InAfter))
			{
				Packages.front() = Owner.GetPackage();
			}
			auto GetDescription() const -> std::string_view override { return "Edit Material Declarations"; }
			auto GetOwningModule() const -> std::string_view override { return "MaterialEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto Undo() -> bool override { return Apply(Before); }
			auto Redo() -> bool override { return Apply(After); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (const auto* State : {&Before, &After})
					for (const auto& Definition : State->Definitions)
						if (DObject* Texture = Definition.Value.TextureValue.Get())
							Collector.AddReferencedObject(Texture);
			}
			auto GetAllocatedSize() const -> size_t override
			{
				size_t Bytes = 0;
				for (const auto* State : {&Before, &After})
				{
					Bytes += State->Definitions.capacity() * sizeof(FMaterialParameterDefinition)
						+ GetMaterialProgramAllocatedSize(State->Program)
						+ GetMaterialGraphPresentationAllocatedSize(State->Presentation);
					for (const auto& Definition : State->Definitions)
						Bytes += Definition.DisplayName.capacity();
				}
				return Bytes;
			}
		private:
			auto Apply(const FState& State) -> bool
			{
				auto* Owner = Material.Get();
				return Owner && Owner->SetMaterialDefinitionsAndProgram(
					State.Definitions, State.Program)
					&& Owner->SetMaterialGraphPresentation(State.Presentation);
			}
			TWeakObjectPtr<DMaterial> Material;
			FState Before;
			FState After;
			std::array<DPackage*, 1> Packages{};
		};

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

		struct FMaterialGraphOutputPresentation
		{
			bool bHasPosition = false;
			int32 X = 0;
			int32 Y = 0;

			auto operator==(const FMaterialGraphOutputPresentation&) const
				-> bool = default;
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

				BeforeOutput = {
					BeforePresentation.bHasMaterialOutputPosition,
					BeforePresentation.MaterialOutputX,
					BeforePresentation.MaterialOutputY};
				AfterOutput = {
					AfterPresentation.bHasMaterialOutputPosition,
					AfterPresentation.MaterialOutputX,
					AfterPresentation.MaterialOutputY};
				bOutputChanged = BeforeOutput != AfterOutput;
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
				if (bOutputChanged)
				{
					const FMaterialGraphOutputPresentation& Output =
						bBefore ? BeforeOutput : AfterOutput;
					Candidate.bHasMaterialOutputPosition = Output.bHasPosition;
					Candidate.MaterialOutputX = Output.X;
					Candidate.MaterialOutputY = Output.Y;
				}
				return Target->SetMaterialGraphPresentation(std::move(Candidate));
			}

			TWeakObjectPtr<DMaterial> Material;
			std::vector<FMaterialGraphNodePresentationChange> NodeChanges;
			FMaterialGraphOutputPresentation BeforeOutput;
			FMaterialGraphOutputPresentation AfterOutput;
			bool bOutputChanged = false;
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
				for (DObject* Object : {
					static_cast<DObject*>(BeforeValue.TextureValue.Get()),
					static_cast<DObject*>(AfterValue.TextureValue.Get())})
					if (Object) Collector.AddReferencedObject(Object);
			}

		private:
			auto Apply(const FMaterialParameterValue& Value) -> bool
			{
				DMaterial* Target = Material.Get();
				return Target && Target->SetParameterValue(ParameterId, Value);
			}

			TWeakObjectPtr<DMaterial> Material;
			FGuid ParameterId;
			FMaterialParameterValue BeforeValue;
			FMaterialParameterValue AfterValue;
			std::array<DPackage*, 1> AffectedPackages{};
		};

	}

	namespace GraphEditInternals
	{
		auto MakeMaterialGraphSemanticTransaction(
			DMaterial& Material,
			FMaterialProgram BeforeProgram,
			FMaterialGraphPresentation BeforePresentation,
			FMaterialProgram AfterProgram,
			FMaterialGraphPresentation AfterPresentation,
			std::string Description,
			FGuid ParameterId,
			FMaterialParameterValue BeforeParameterValue,
			FMaterialParameterValue AfterParameterValue)
			-> std::unique_ptr<ITransactionCustomChange>
		{
			return std::make_unique<FMaterialGraphSemanticTransaction>(
				Material, std::move(BeforeProgram), std::move(BeforePresentation),
				std::move(AfterProgram), std::move(AfterPresentation),
				std::move(Description), ParameterId,
				std::move(BeforeParameterValue), std::move(AfterParameterValue));
		}

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

		auto MakeMaterialDeclarationTransaction(DMaterial& Material,
			FMaterialDeclarationState Before, FMaterialDeclarationState After)
			-> std::unique_ptr<ITransactionCustomChange>
		{
			return std::make_unique<FMaterialDeclarationTransaction>(
				Material, std::move(Before), std::move(After));
		}

		auto CommitSemanticChange(
			DMaterial& Material,
			FMaterialProgram CandidateProgram,
			FMaterialGraphPresentation CandidatePresentation,
			std::string Description,
			std::vector<FGuid> Affected,
			std::vector<FGuid> Generated,
			DTransactor* Transactions)
			-> FMaterialGraphCommandResult
		{
			if (!IsValid(&Material))
				return {.Status = EMaterialGraphCommandStatus::StaleOwner,
					.Message = "The material graph owner is no longer available."};
			if (Transactions && Transactions->HasPendingOperation())
				return MakeRejected("The editor transactor is busy.");
			const FMaterialProgram BeforeProgram = *Material.GetMaterialProgram();
			const FMaterialGraphPresentation BeforePresentation =
				Material.GetMaterialGraphPresentation();
			CandidatePresentation = SanitizeMaterialGraphPresentation(
				CandidatePresentation, CandidateProgram);
			if (BeforeProgram == CandidateProgram
				&& BeforePresentation == CandidatePresentation)
			{
				return {.Status = EMaterialGraphCommandStatus::NoChange};
			}

			FMaterialProgramValidationResult Validation = ValidateMaterialProgram(
				CandidateProgram, Material.GetParameterDefinitions());
			if (!Validation)
				return MakeRejected("The material graph command produced an invalid program.",
					std::move(Validation.Diagnostics));

			if (!(Validation = Material.SetMaterialProgram(CandidateProgram)))
				return MakeRejected("The material rejected the candidate program.",
					std::move(Validation.Diagnostics));
			if (!Material.SetMaterialGraphPresentation(CandidatePresentation))
			{
				const auto RollbackValidation = Material.SetMaterialProgram(BeforeProgram);
				return MakeRejected("The material rejected the candidate graph presentation.");
			}

			if (Transactions)
			{
				const auto bRecorded = Transactions->CommitApplied(
					std::make_unique<FMaterialGraphSemanticTransaction>(
					Material,
					BeforeProgram,
					BeforePresentation,
					std::move(CandidateProgram),
					std::move(CandidatePresentation),
					std::move(Description)));
				check(bRecorded);
			}
			std::ranges::sort(Affected);
			Affected.erase(std::unique(Affected.begin(), Affected.end()), Affected.end());
			return {
				.Status = EMaterialGraphCommandStatus::Succeeded,
				.AffectedNodeIds = std::move(Affected),
				.GeneratedNodeIds = std::move(Generated),
			};
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
