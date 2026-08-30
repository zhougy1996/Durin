#include "MaterialGraphOperations.h"

#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"
#include "Editor/Transactor.h"
#include "Misc/StringHelper.h"

namespace Durin::Editor::Material
{
	namespace
	{
		constexpr std::array NumericTypes{
			EMaterialProgramValueType::Float,
			EMaterialProgramValueType::Float2,
			EMaterialProgramValueType::Float3,
			EMaterialProgramValueType::Float4,
		};

		auto GetInputNames(EMaterialProgramOpcode Opcode, size_t Count)
			-> std::vector<std::string>
		{
			std::vector<std::string> Names;
			switch (Opcode)
			{
			case EMaterialProgramOpcode::TextureSample2D: Names = {"Texture", "UV"}; break;
			case EMaterialProgramOpcode::Add:
			case EMaterialProgramOpcode::Subtract:
			case EMaterialProgramOpcode::Multiply:
			case EMaterialProgramOpcode::Divide:
			case EMaterialProgramOpcode::Minimum:
			case EMaterialProgramOpcode::Maximum: Names = {"A", "B"}; break;
			case EMaterialProgramOpcode::Clamp: Names = {"Value", "Min", "Max"}; break;
			case EMaterialProgramOpcode::Lerp: Names = {"A", "B", "Alpha"}; break;
			case EMaterialProgramOpcode::MakeFloat2: Names = {"X", "Y"}; break;
			case EMaterialProgramOpcode::MakeFloat3: Names = {"X", "Y", "Z"}; break;
			case EMaterialProgramOpcode::MakeFloat4: Names = {"X", "Y", "Z", "W"}; break;
			case EMaterialProgramOpcode::BlendNormalsRNM: Names = {"Base", "Detail"}; break;
			default: break;
			}
			if (Names.size() < Count) Names.resize(Count, "Value");
			if (Names.size() > Count) Names.resize(Count);
			return Names;
		}

		auto GetCategory(EMaterialProgramOpcode Opcode) -> const char*
		{
			switch (Opcode)
			{
			case EMaterialProgramOpcode::Constant:
			case EMaterialProgramOpcode::Parameter:
			case EMaterialProgramOpcode::TextureParameter:
			case EMaterialProgramOpcode::TextureCoordinate: return "Inputs";
			case EMaterialProgramOpcode::TextureSample2D:
			case EMaterialProgramOpcode::DecodeNormalRG:
			case EMaterialProgramOpcode::BlendNormalsRNM: return "Textures";
			case EMaterialProgramOpcode::StandardSurface: return "Surface";
			case EMaterialProgramOpcode::Swizzle:
			case EMaterialProgramOpcode::MakeFloat2:
			case EMaterialProgramOpcode::MakeFloat3:
			case EMaterialProgramOpcode::MakeFloat4:
			case EMaterialProgramOpcode::Splat2:
			case EMaterialProgramOpcode::Splat3:
			case EMaterialProgramOpcode::Splat4:
			case EMaterialProgramOpcode::TruncateToFloat:
			case EMaterialProgramOpcode::TruncateToFloat2:
			case EMaterialProgramOpcode::TruncateToFloat3: return "Channels";
			default: return "Math";
			}
		}

		auto GetProgramType(EMaterialParameterType Type)
			-> EMaterialProgramValueType
		{
			switch (Type)
			{
			case EMaterialParameterType::Scalar: return EMaterialProgramValueType::Float;
			case EMaterialParameterType::Vector2: return EMaterialProgramValueType::Float2;
			case EMaterialParameterType::Vector: return EMaterialProgramValueType::Float3;
			case EMaterialParameterType::Texture: return EMaterialProgramValueType::Texture2D;
			}
			return EMaterialProgramValueType::Float;
		}

		auto GetProgramTypeName(EMaterialProgramValueType Type) -> const char*
		{
			switch (Type)
			{
			case EMaterialProgramValueType::Float: return "Float";
			case EMaterialProgramValueType::Float2: return "Float2";
			case EMaterialProgramValueType::Float3: return "Float3";
			case EMaterialProgramValueType::Float4: return "Float4";
			case EMaterialProgramValueType::Texture2D: return "Texture2D";
			case EMaterialProgramValueType::Surface: return "Surface";
			}
			return "Unknown";
		}

		auto GetOpcodeName(EMaterialProgramOpcode Opcode) -> const char*
		{
			switch (Opcode)
			{
			case EMaterialProgramOpcode::Constant: return "Constant";
			case EMaterialProgramOpcode::Parameter: return "Parameter";
			case EMaterialProgramOpcode::TextureParameter: return "Texture Parameter";
			case EMaterialProgramOpcode::TextureCoordinate: return "Texture Coordinate";
			case EMaterialProgramOpcode::TextureSample2D: return "Texture Sample 2D";
			case EMaterialProgramOpcode::Add: return "Add";
			case EMaterialProgramOpcode::Subtract: return "Subtract";
			case EMaterialProgramOpcode::Multiply: return "Multiply";
			case EMaterialProgramOpcode::Divide: return "Divide";
			case EMaterialProgramOpcode::Minimum: return "Minimum";
			case EMaterialProgramOpcode::Maximum: return "Maximum";
			case EMaterialProgramOpcode::Negate: return "Negate";
			case EMaterialProgramOpcode::OneMinus: return "One Minus";
			case EMaterialProgramOpcode::Absolute: return "Absolute";
			case EMaterialProgramOpcode::Saturate: return "Saturate";
			case EMaterialProgramOpcode::Normalize: return "Normalize";
			case EMaterialProgramOpcode::Clamp: return "Clamp";
			case EMaterialProgramOpcode::Lerp: return "Lerp";
			case EMaterialProgramOpcode::MakeFloat2: return "Make Float2";
			case EMaterialProgramOpcode::MakeFloat3: return "Make Float3";
			case EMaterialProgramOpcode::MakeFloat4: return "Make Float4";
			case EMaterialProgramOpcode::Swizzle: return "Swizzle";
			case EMaterialProgramOpcode::Splat2: return "Splat2";
			case EMaterialProgramOpcode::Splat3: return "Splat3";
			case EMaterialProgramOpcode::Splat4: return "Splat4";
			case EMaterialProgramOpcode::TruncateToFloat: return "Truncate to Float";
			case EMaterialProgramOpcode::TruncateToFloat2: return "Truncate to Float2";
			case EMaterialProgramOpcode::TruncateToFloat3: return "Truncate to Float3";
			case EMaterialProgramOpcode::DecodeNormalRG: return "Decode Normal RG";
			case EMaterialProgramOpcode::BlendNormalsRNM: return "Blend Normals RNM";
			case EMaterialProgramOpcode::StandardSurface: return "Standard Surface";
			}
			return "Unknown";
		}

		auto MakeCatalogEntry(
			EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType ResultType,
			std::vector<std::vector<EMaterialProgramValueType>> Inputs = {})
			-> FMaterialGraphCatalogEntry
		{
			FMaterialGraphCatalogEntry Entry;
			Entry.Name = GetOpcodeName(Opcode);
			Entry.OperationName = Entry.Name;
			Entry.Category = GetCategory(Opcode);
			switch (Opcode)
			{
			case EMaterialProgramOpcode::Constant: Entry.Description = "A literal numeric value."; break;
			case EMaterialProgramOpcode::Parameter:
			case EMaterialProgramOpcode::TextureParameter: Entry.Description = "A value exposed by the material parameter definition."; break;
			case EMaterialProgramOpcode::TextureCoordinate: Entry.Description = "Texture coordinates associated with a texture parameter."; break;
			case EMaterialProgramOpcode::TextureSample2D: Entry.Description = "Samples a 2D texture at the supplied coordinates."; break;
			case EMaterialProgramOpcode::Add: Entry.Description = "Adds two values component by component."; break;
			case EMaterialProgramOpcode::Subtract: Entry.Description = "Subtracts the second value from the first."; break;
			case EMaterialProgramOpcode::Multiply: Entry.Description = "Multiplies two values component by component."; break;
			case EMaterialProgramOpcode::Divide: Entry.Description = "Divides the first value by the second."; break;
			case EMaterialProgramOpcode::Minimum: Entry.Description = "Returns the component-wise minimum."; break;
			case EMaterialProgramOpcode::Maximum: Entry.Description = "Returns the component-wise maximum."; break;
			case EMaterialProgramOpcode::Negate: Entry.Description = "Reverses the sign of a value."; break;
			case EMaterialProgramOpcode::OneMinus: Entry.Description = "Subtracts a value from one."; break;
			case EMaterialProgramOpcode::Absolute: Entry.Description = "Returns the absolute value."; break;
			case EMaterialProgramOpcode::Saturate: Entry.Description = "Clamps a value to the zero-to-one range."; break;
			case EMaterialProgramOpcode::Normalize: Entry.Description = "Returns a unit-length vector."; break;
			case EMaterialProgramOpcode::Clamp: Entry.Description = "Constrains a value between minimum and maximum inputs."; break;
			case EMaterialProgramOpcode::Lerp: Entry.Description = "Interpolates between two values."; break;
			case EMaterialProgramOpcode::MakeFloat2:
			case EMaterialProgramOpcode::MakeFloat3:
			case EMaterialProgramOpcode::MakeFloat4: Entry.Description = "Combines scalar inputs into a vector."; break;
			case EMaterialProgramOpcode::Swizzle: Entry.Description = "Reorders or selects vector components."; break;
			case EMaterialProgramOpcode::Splat2:
			case EMaterialProgramOpcode::Splat3:
			case EMaterialProgramOpcode::Splat4: Entry.Description = "Replicates a scalar across vector components."; break;
			case EMaterialProgramOpcode::TruncateToFloat:
			case EMaterialProgramOpcode::TruncateToFloat2:
			case EMaterialProgramOpcode::TruncateToFloat3: Entry.Description = "Keeps the leading components of a wider vector."; break;
			case EMaterialProgramOpcode::DecodeNormalRG: Entry.Description = "Reconstructs a tangent-space normal from two channels."; break;
			case EMaterialProgramOpcode::BlendNormalsRNM: Entry.Description = "Blends two tangent-space normals with RNM."; break;
			case EMaterialProgramOpcode::StandardSurface: Entry.Description = "Builds the Engine standard PBR surface from canonical material parameters."; break;
			}
			Entry.NodeTemplate.Opcode = Opcode;
			Entry.NodeTemplate.ResultType = ResultType;
			Entry.NodeTemplate.Inputs.resize(Inputs.size());
			Entry.InputNames = GetInputNames(Opcode, Inputs.size());
			Entry.AcceptedInputTypes = std::move(Inputs);
			return Entry;
		}

		auto NormalizeSearchText(std::string_view Value) -> std::string
		{
			return StringUtils::FoldAscii(Value);
		}

		auto PrepareSearchFields(FMaterialGraphCatalogEntry& Entry) -> void
		{
			Entry.NormalizedSearchFields = {
				NormalizeSearchText(Entry.OperationName),
				NormalizeSearchText(Entry.SecondaryName),
				NormalizeSearchText(Entry.Category),
				NormalizeSearchText(Entry.Description),
				NormalizeSearchText(GetProgramTypeName(Entry.NodeTemplate.ResultType)),
			};
		}

		auto FindNode(FMaterialProgram& Program, const FGuid& Id)
			-> FMaterialProgramNode*
		{
			const auto It = std::ranges::find(Program.Nodes, Id,
				&FMaterialProgramNode::Id);
			return It == Program.Nodes.end() ? nullptr : &*It;
		}

		auto FindNode(const FMaterialProgram& Program, const FGuid& Id)
			-> const FMaterialProgramNode*
		{
			const auto It = std::ranges::find(Program.Nodes, Id,
				&FMaterialProgramNode::Id);
			return It == Program.Nodes.end() ? nullptr : &*It;
		}

		auto MakeParameterValue(
			EMaterialProgramValueType Type,
			const FMaterialProgramLiteral& Literal) -> FMaterialParameterValue
		{
			switch (Type)
			{
			case EMaterialProgramValueType::Float:
				return FMaterialParameterValue::MakeScalar(Literal.X);
			case EMaterialProgramValueType::Float2:
				return FMaterialParameterValue::MakeVector2({Literal.X, Literal.Y});
			case EMaterialProgramValueType::Float3:
				return FMaterialParameterValue::MakeVector(
					{Literal.X, Literal.Y, Literal.Z});
			default: return {};
			}
		}

		auto MakeRejected(
			std::string Message,
			std::vector<FMaterialProgramDiagnostic> Diagnostics = {})
			-> FMaterialGraphCommandResult
		{
			return {
				.Status = EMaterialGraphCommandStatus::Rejected,
				.Diagnostics = std::move(Diagnostics),
				.Message = std::move(Message),
			};
		}

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
				if (!Target->SetMaterialProgram(Program, Validation)) return false;
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

			if (!Material.SetMaterialProgram(CandidateProgram, Validation))
				return MakeRejected("The material rejected the candidate program.",
					std::move(Validation.Diagnostics));
			if (!Material.SetMaterialGraphPresentation(CandidatePresentation))
			{
				FMaterialProgramValidationResult RollbackValidation;
				Material.SetMaterialProgram(BeforeProgram, RollbackValidation);
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

	auto FMaterialGraphGeometry::GetMetrics()
		-> const FMaterialGraphCanvasMetrics&
	{
		static constexpr FMaterialGraphCanvasMetrics Metrics;
		return Metrics;
	}

	auto FMaterialGraphGeometry::GetNodeHeight(uint32 InputCount) -> float
	{
		const FMaterialGraphCanvasMetrics& Metrics = GetMetrics();
		return Metrics.HeaderHeight + Metrics.SecondaryHeight
			+ Metrics.BodyPadding * 2.0f
			+ Metrics.PinRowHeight * std::max(1u, InputCount);
	}

	auto FMaterialGraphGeometry::GetSurfacePinOffset(uint32 InputIndex) -> float
	{
		const FMaterialGraphCanvasMetrics& Metrics = GetMetrics();
		return Metrics.SurfaceHeaderHeight
			+ Metrics.PinRowHeight * (static_cast<float>(InputIndex) + 0.5f);
	}

	auto FMaterialGraphGeometry::SelectDetailLevel(
		float Zoom,
		EMaterialGraphDetailLevel Previous) -> EMaterialGraphDetailLevel
	{
		if (Previous == EMaterialGraphDetailLevel::Overview)
			return Zoom > 0.48f ? EMaterialGraphDetailLevel::Readable : Previous;
		if (Previous == EMaterialGraphDetailLevel::Editing)
			return Zoom < 0.74f ? EMaterialGraphDetailLevel::Readable : Previous;
		if (Zoom < 0.42f) return EMaterialGraphDetailLevel::Overview;
		if (Zoom > 0.82f) return EMaterialGraphDetailLevel::Editing;
		return EMaterialGraphDetailLevel::Readable;
	}

	auto FMaterialGraphOperations::Inspect(const DMaterial& Material)
		-> FMaterialGraphView
	{
		const std::vector Catalog = EnumerateCatalog(Material);
		return Inspect(Material, Catalog);
	}

	auto FMaterialGraphOperations::Inspect(
		const DMaterial& Material,
		std::span<const FMaterialGraphCatalogEntry> Catalog)
		-> FMaterialGraphView
	{
		struct FCatalogShapeKey
		{
			EMaterialProgramOpcode Opcode;
			EMaterialProgramValueType ResultType;
			FGuid ParameterId;

			auto operator==(const FCatalogShapeKey&) const -> bool = default;
		};
		struct FCatalogShapeKeyHash
		{
			auto operator()(const FCatalogShapeKey& Key) const -> size_t
			{
				size_t Hash = static_cast<size_t>(Key.Opcode);
				Hash = Hash * 31 + static_cast<size_t>(Key.ResultType);
				return Hash * 31 + std::hash<FGuid>{}(Key.ParameterId);
			}
		};
		const auto BaseShapeKey = [](EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType ResultType) {
			return static_cast<uint32>(Opcode) << 8
				| static_cast<uint32>(ResultType);
		};

		FMaterialGraphView Result;
		const FMaterialProgram& Program = *Material.GetMaterialProgram();
		const FMaterialGraphPresentation Presentation =
			SanitizeMaterialGraphPresentation(
				Material.GetMaterialGraphPresentation(), Program);
		std::unordered_map<FGuid, const FMaterialProgramNode*> NodesById;
		NodesById.reserve(Program.Nodes.size());
		for (const FMaterialProgramNode& Node : Program.Nodes)
			NodesById.emplace(Node.Id, &Node);
		std::unordered_map<uint32, const FMaterialGraphCatalogEntry*> BaseShapes;
		std::unordered_map<FCatalogShapeKey,
			const FMaterialGraphCatalogEntry*, FCatalogShapeKeyHash> ParameterShapes;
		BaseShapes.reserve(Catalog.size());
		ParameterShapes.reserve(Catalog.size());
		for (const FMaterialGraphCatalogEntry& Entry : Catalog)
		{
			BaseShapes.emplace(BaseShapeKey(
				Entry.NodeTemplate.Opcode, Entry.NodeTemplate.ResultType), &Entry);
			if (Entry.NodeTemplate.ParameterId.IsValid())
				ParameterShapes.emplace(FCatalogShapeKey{
					Entry.NodeTemplate.Opcode,
					Entry.NodeTemplate.ResultType,
					Entry.NodeTemplate.ParameterId}, &Entry);
		}
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		Positions.reserve(Presentation.Nodes.size());
		for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			Positions.emplace(Position.NodeId, Position);
		Result.Nodes.reserve(Program.Nodes.size());
		for (const FMaterialProgramNode& Node : Program.Nodes)
		{
			FMaterialGraphNodeView View{.Node = Node};
			const FMaterialGraphCatalogEntry* Shape = nullptr;
			if (Node.ParameterId.IsValid())
			{
				const auto It = ParameterShapes.find({
					Node.Opcode, Node.ResultType, Node.ParameterId});
				if (It != ParameterShapes.end()) Shape = It->second;
			}
			else
			{
				const auto It = BaseShapes.find(BaseShapeKey(
					Node.Opcode, Node.ResultType));
				if (It != BaseShapes.end()) Shape = It->second;
			}
			View.PrimaryLabel = Shape
				? Shape->OperationName : GetOpcodeName(Node.Opcode);
			View.SecondaryLabel = Node.DisplayName;
			if (View.SecondaryLabel.empty() && Shape)
				View.SecondaryLabel = Shape->SecondaryName;
			View.Inputs.reserve(Node.Inputs.size());
			for (uint32 InputIndex = 0; InputIndex < Node.Inputs.size(); ++InputIndex)
			{
				const auto SourceIt = NodesById.find(
					Node.Inputs[InputIndex].SourceNodeId);
				const FMaterialProgramNode* Source = SourceIt == NodesById.end()
					? nullptr : SourceIt->second;
				FMaterialGraphPinView Pin{
					.InputIndex = InputIndex,
					.Name = Shape
						&& InputIndex < Shape->InputNames.size()
						? Shape->InputNames[InputIndex] : "Value",
					.Link = Node.Inputs[InputIndex],
					.SourceType = Source ? Source->ResultType
						: EMaterialProgramValueType::Float,
				};
				if (Shape
					&& InputIndex < Shape->AcceptedInputTypes.size())
					Pin.AcceptedTypes = Shape->AcceptedInputTypes[InputIndex];
				View.Inputs.push_back(std::move(Pin));
			}
			const auto It = Positions.find(Node.Id);
			check(It != Positions.end());
			View.Presentation = It->second;
			Result.Nodes.push_back(std::move(View));
		}
		std::ranges::sort(Result.Nodes, {}, [](const FMaterialGraphNodeView& View) {
			return View.Node.Id;
		});
		Result.Outputs = Program.Outputs;
		check(Presentation.bHasMaterialOutputPosition);
		Result.MaterialOutputPosition = {
			Presentation.MaterialOutputX,
			Presentation.MaterialOutputY};
		return Result;
	}

	auto FMaterialGraphOperations::EnumerateCatalog(const DMaterial& Material)
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		std::vector<FMaterialGraphCatalogEntry> Result;
		const auto One = [](EMaterialProgramValueType Type) {
			return std::vector<EMaterialProgramValueType>{Type};
		};
		const auto Numeric = [] {
			return std::vector<EMaterialProgramValueType>(
				NumericTypes.begin(), NumericTypes.end());
		};
		for (EMaterialProgramValueType Type : NumericTypes)
			Result.push_back(MakeCatalogEntry(
				EMaterialProgramOpcode::Constant, Type));
		for (const FMaterialParameterDefinition& Definition
			: Material.GetParameterDefinitions())
		{
			const EMaterialProgramValueType Type = GetProgramType(Definition.Type);
			FMaterialGraphCatalogEntry Parameter = MakeCatalogEntry(
				Type == EMaterialProgramValueType::Texture2D
					? EMaterialProgramOpcode::TextureParameter
					: EMaterialProgramOpcode::Parameter,
				Type);
			Parameter.Name = Definition.DisplayName;
			Parameter.SecondaryName = Definition.DisplayName;
			Parameter.NodeTemplate.ParameterId = Definition.Id;
			Parameter.NodeTemplate.DisplayName = Definition.DisplayName;
			Result.push_back(std::move(Parameter));
			if (Type == EMaterialProgramValueType::Texture2D)
			{
				FMaterialGraphCatalogEntry Coordinates = MakeCatalogEntry(
					EMaterialProgramOpcode::TextureCoordinate,
					EMaterialProgramValueType::Float2);
				Coordinates.Name = std::format("{} UV", Definition.DisplayName);
				Coordinates.SecondaryName = Definition.DisplayName;
				Coordinates.NodeTemplate.ParameterId = Definition.Id;
				Result.push_back(std::move(Coordinates));
			}
		}
		Result.push_back(MakeCatalogEntry(
			EMaterialProgramOpcode::TextureSample2D,
			EMaterialProgramValueType::Float4,
			{One(EMaterialProgramValueType::Texture2D),
				One(EMaterialProgramValueType::Float2)}));
		for (EMaterialProgramOpcode Opcode : {
			EMaterialProgramOpcode::Add, EMaterialProgramOpcode::Subtract,
			EMaterialProgramOpcode::Multiply, EMaterialProgramOpcode::Divide,
			EMaterialProgramOpcode::Minimum, EMaterialProgramOpcode::Maximum})
			for (EMaterialProgramValueType Type : NumericTypes)
				Result.push_back(MakeCatalogEntry(Opcode, Type, {One(Type), One(Type)}));
		for (EMaterialProgramOpcode Opcode : {
			EMaterialProgramOpcode::Negate, EMaterialProgramOpcode::OneMinus,
			EMaterialProgramOpcode::Absolute, EMaterialProgramOpcode::Saturate})
			for (EMaterialProgramValueType Type : NumericTypes)
				Result.push_back(MakeCatalogEntry(Opcode, Type, {One(Type)}));
		for (EMaterialProgramValueType Type : NumericTypes | std::views::drop(1))
			Result.push_back(MakeCatalogEntry(
				EMaterialProgramOpcode::Normalize, Type, {One(Type)}));
		for (EMaterialProgramValueType Type : NumericTypes)
		{
			Result.push_back(MakeCatalogEntry(
				EMaterialProgramOpcode::Clamp, Type,
				{One(Type), One(Type), One(Type)}));
			Result.push_back(MakeCatalogEntry(
				EMaterialProgramOpcode::Lerp, Type,
				{One(Type), One(Type), One(EMaterialProgramValueType::Float)}));
		}
		for (uint8 Width = 2; Width <= 4; ++Width)
		{
			const EMaterialProgramValueType Type = NumericTypes[Width - 1];
			std::vector<std::vector<EMaterialProgramValueType>> Inputs(
				Width, One(EMaterialProgramValueType::Float));
			Result.push_back(MakeCatalogEntry(
				static_cast<EMaterialProgramOpcode>(
					static_cast<uint8>(EMaterialProgramOpcode::MakeFloat2) + Width - 2),
				Type, std::move(Inputs)));
			Result.push_back(MakeCatalogEntry(
				static_cast<EMaterialProgramOpcode>(
					static_cast<uint8>(EMaterialProgramOpcode::Splat2) + Width - 2),
				Type, {One(EMaterialProgramValueType::Float)}));
		}
		for (uint8 Width = 1; Width <= 4; ++Width)
		{
			FMaterialGraphCatalogEntry Swizzle = MakeCatalogEntry(
				EMaterialProgramOpcode::Swizzle, NumericTypes[Width - 1], {Numeric()});
			Swizzle.NodeTemplate.SwizzleLength = Width;
			Swizzle.NodeTemplate.SwizzleX = 0;
			Swizzle.NodeTemplate.SwizzleY = std::min<uint8>(1, Width - 1);
			Swizzle.NodeTemplate.SwizzleZ = std::min<uint8>(2, Width - 1);
			Swizzle.NodeTemplate.SwizzleW = std::min<uint8>(3, Width - 1);
			Result.push_back(std::move(Swizzle));
		}
		for (uint8 Width = 1; Width <= 3; ++Width)
		{
			std::vector<EMaterialProgramValueType> Wider(
				NumericTypes.begin() + Width, NumericTypes.end());
			Result.push_back(MakeCatalogEntry(
				static_cast<EMaterialProgramOpcode>(
					static_cast<uint8>(EMaterialProgramOpcode::TruncateToFloat) + Width - 1),
				NumericTypes[Width - 1], {std::move(Wider)}));
		}
		Result.push_back(MakeCatalogEntry(
			EMaterialProgramOpcode::DecodeNormalRG,
			EMaterialProgramValueType::Float3,
			{One(EMaterialProgramValueType::Float2)}));
		Result.push_back(MakeCatalogEntry(
			EMaterialProgramOpcode::BlendNormalsRNM,
			EMaterialProgramValueType::Float3,
			{One(EMaterialProgramValueType::Float3),
				One(EMaterialProgramValueType::Float3)}));
		Result.push_back(MakeCatalogEntry(
			EMaterialProgramOpcode::StandardSurface,
			EMaterialProgramValueType::Surface));
		std::ranges::stable_sort(Result, {}, &FMaterialGraphCatalogEntry::Name);
		for (FMaterialGraphCatalogEntry& Entry : Result) PrepareSearchFields(Entry);
		return Result;
	}

	auto FMaterialGraphOperations::SearchCatalog(
		const DMaterial& Material,
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType)
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		return SearchCatalog(EnumerateCatalog(Material), Query, SourceType);
	}

	auto FMaterialGraphOperations::SearchCatalog(
		std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType)
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		const std::vector<size_t> Indices = SearchCatalogIndices(
			Catalog, Query, SourceType);
		std::vector<FMaterialGraphCatalogEntry> Result;
		Result.reserve(Indices.size());
		for (const size_t Index : Indices) Result.push_back(Catalog[Index]);
		return Result;
	}

	auto FMaterialGraphOperations::SearchCatalogIndices(
		std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType)
		-> std::vector<size_t>
	{
		const std::string Needle = NormalizeSearchText(Query);
		struct FRankedEntry
		{
			size_t Index = 0;
			uint8 Match = 3;
			size_t Ordinal = 0;
		};
		std::vector<FRankedEntry> Ranked;
		for (size_t Ordinal = 0; Ordinal < Catalog.size(); ++Ordinal)
		{
			const FMaterialGraphCatalogEntry& Entry = Catalog[Ordinal];
			if (SourceType)
			{
				if (Entry.AcceptedInputTypes.empty()
					|| std::ranges::find(Entry.AcceptedInputTypes.front(), *SourceType)
						== Entry.AcceptedInputTypes.front().end()) continue;
			}
			uint8 Match = Needle.empty() ? 3 : 4;
			std::array<std::string, 5> FallbackSearchFields;
			const std::array<std::string, 5>* SearchFields =
				&Entry.NormalizedSearchFields;
			if (std::ranges::all_of(*SearchFields,
				[](const std::string& Field) { return Field.empty(); }))
			{
				FallbackSearchFields = {
					NormalizeSearchText(Entry.OperationName),
					NormalizeSearchText(Entry.SecondaryName),
					NormalizeSearchText(Entry.Category),
					NormalizeSearchText(Entry.Description),
					NormalizeSearchText(GetProgramTypeName(Entry.NodeTemplate.ResultType)),
				};
				SearchFields = &FallbackSearchFields;
			}
			for (const std::string& Haystack : *SearchFields)
			{
				if (Haystack == Needle) Match = std::min<uint8>(Match, 0);
				else if (Haystack.starts_with(Needle)) Match = std::min<uint8>(Match, 1);
				else if (Haystack.find(Needle) != std::string::npos)
					Match = std::min<uint8>(Match, 2);
			}
			if (Match < 4) Ranked.push_back({Ordinal, Match, Ordinal});
		}
		std::ranges::sort(Ranked, [Catalog](const FRankedEntry& A,
			const FRankedEntry& B) {
			if (A.Match != B.Match) return A.Match < B.Match;
			const FMaterialGraphCatalogEntry& EntryA = Catalog[A.Index];
			const FMaterialGraphCatalogEntry& EntryB = Catalog[B.Index];
			if (EntryA.Category != EntryB.Category)
				return EntryA.Category < EntryB.Category;
			if (EntryA.OperationName != EntryB.OperationName)
				return EntryA.OperationName < EntryB.OperationName;
			if (EntryA.NodeTemplate.ResultType != EntryB.NodeTemplate.ResultType)
				return EntryA.NodeTemplate.ResultType < EntryB.NodeTemplate.ResultType;
			if (EntryA.NodeTemplate.ParameterId != EntryB.NodeTemplate.ParameterId)
				return EntryA.NodeTemplate.ParameterId < EntryB.NodeTemplate.ParameterId;
			return A.Ordinal < B.Ordinal;
		});
		std::vector<size_t> Result;
		Result.reserve(Ranked.size());
		for (const FRankedEntry& Entry : Ranked) Result.push_back(Entry.Index);
		return Result;
	}

	auto FMaterialGraphOperations::CreateNode(
		DMaterial& Material,
		FMaterialGraphCreateNodeRequest Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (Candidate.Nodes.size() >= MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph node limit has been reached.");
		if (!Request.Node.Id.IsValid()) Request.Node.Id = FGuid::NewGuid();
		if (FindNode(Candidate, Request.Node.Id))
			return MakeRejected("The requested material graph node GUID already exists.");
		const FGuid GeneratedId = Request.Node.Id;
		Candidate.Nodes.push_back(std::move(Request.Node));
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		Presentation.Nodes.push_back({GeneratedId, Request.X, Request.Y});
		return CommitSemanticChange(Material, std::move(Candidate), std::move(Presentation),
			"Create Material Node", {GeneratedId}, {GeneratedId}, Transactions);
	}

	auto FMaterialGraphOperations::CreateNodeWithDefaultInputs(
		DMaterial& Material,
		FMaterialGraphCreateNodeRequest Request,
		std::span<const std::vector<EMaterialProgramValueType>> AcceptedInputTypes,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (AcceptedInputTypes.size() != Request.Node.Inputs.size())
			return MakeRejected("The node palette input shape is stale.");
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (!Request.Node.Id.IsValid()) Request.Node.Id = FGuid::NewGuid();
		if (FindNode(Candidate, Request.Node.Id))
			return MakeRejected("The requested material graph node GUID already exists.");

		std::vector<FMaterialProgramNode> Defaults;
		for (size_t InputIndex = 0; InputIndex < Request.Node.Inputs.size(); ++InputIndex)
		{
			if (Request.Node.Inputs[InputIndex].SourceNodeId.IsValid()) continue;
			const auto NumericType = std::ranges::find_if(AcceptedInputTypes[InputIndex],
				[](EMaterialProgramValueType Type) {
					return Type != EMaterialProgramValueType::Texture2D;
				});
			if (NumericType == AcceptedInputTypes[InputIndex].end())
				return MakeRejected("This node requires a resource input that has no default value.");
			FMaterialProgramNode Default;
			Default.Id = FGuid::NewGuid();
			Default.Opcode = EMaterialProgramOpcode::Constant;
			Default.ResultType = *NumericType;
			float Value = 0.0f;
			if ((Request.Node.Opcode == EMaterialProgramOpcode::Multiply
				|| Request.Node.Opcode == EMaterialProgramOpcode::Divide)
				&& InputIndex == 1) Value = 1.0f;
			else if (Request.Node.Opcode == EMaterialProgramOpcode::Clamp
				&& InputIndex == 2) Value = 1.0f;
			else if (Request.Node.Opcode == EMaterialProgramOpcode::Lerp)
				Value = InputIndex == 1 ? 1.0f : InputIndex == 2 ? 0.5f : 0.0f;
			else if (Request.Node.Opcode == EMaterialProgramOpcode::Normalize)
				Value = 1.0f;
			Default.Literal = {Value, Value, Value, Value};
			Request.Node.Inputs[InputIndex] = {Default.Id, 0};
			Defaults.push_back(std::move(Default));
		}
		if (Candidate.Nodes.size() + Defaults.size() + 1 > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph node limit has been reached.");

		const FGuid NodeId = Request.Node.Id;
		std::vector<FGuid> Generated{NodeId};
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		const FMaterialGraphCanvasMetrics& Metrics = FMaterialGraphGeometry::GetMetrics();
		for (size_t Index = 0; Index < Defaults.size(); ++Index)
		{
			Generated.push_back(Defaults[Index].Id);
			Presentation.Nodes.push_back({Defaults[Index].Id,
				Request.X - static_cast<int32>(Metrics.NodeWidth + Metrics.ColumnGap),
				Request.Y + static_cast<int32>(Index
					* (FMaterialGraphGeometry::GetNodeHeight(0) + Metrics.RowGap))});
			Candidate.Nodes.push_back(std::move(Defaults[Index]));
		}
		Candidate.Nodes.push_back(std::move(Request.Node));
		Presentation.Nodes.push_back({NodeId, Request.X, Request.Y});
		return CommitSemanticChange(Material, std::move(Candidate), std::move(Presentation),
			"Create Material Node", Generated, Generated, Transactions);
	}

	auto FMaterialGraphOperations::ReplaceNode(
		DMaterial& Material,
		FMaterialProgramNode Node,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		FMaterialProgramNode* Existing = FindNode(Candidate, Node.Id);
		if (!Existing) return MakeRejected("The material graph node does not exist.");
		const FGuid AffectedId = Node.Id;
		*Existing = std::move(Node);
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Edit Material Node", {AffectedId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::RemoveNodes(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (NodeIds.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph removal request exceeds the node bound.");
		std::unordered_set<FGuid> Removed(NodeIds.begin(), NodeIds.end());
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		for (const FMaterialProgramNode& Node : Candidate.Nodes)
		{
			if (Removed.contains(Node.Id)) continue;
			if (std::ranges::any_of(Node.Inputs,
				[&](const FMaterialProgramLink& Link) {
					return Removed.contains(Link.SourceNodeId);
				}))
			{
				return MakeRejected(
					"A material graph node still depends on the requested selection. "
					"Reconnect or remove the dependent node first.");
			}
		}
		const size_t BeforeCount = Candidate.Nodes.size();
		std::erase_if(Candidate.Nodes, [&](const FMaterialProgramNode& Node) {
			return Removed.contains(Node.Id);
		});
		if (Candidate.Nodes.size() == BeforeCount)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		for (EMaterialSurfaceOutput Output : {
			EMaterialSurfaceOutput::BaseColor,
			EMaterialSurfaceOutput::Normal,
			EMaterialSurfaceOutput::Metallic,
			EMaterialSurfaceOutput::Roughness,
			EMaterialSurfaceOutput::AmbientOcclusion,
			EMaterialSurfaceOutput::Emissive,
			EMaterialSurfaceOutput::Opacity,
			EMaterialSurfaceOutput::OpacityMask})
		{
			FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(
				Candidate.Outputs, Output);
			if (Removed.contains(Link.SourceNodeId)) Link = {};
		}
		if (Removed.contains(Candidate.Outputs.Surface.SourceNodeId))
			Candidate.Outputs.Surface = {};
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		std::erase_if(Presentation.Nodes, [&](const FMaterialGraphNodePresentation& Node) {
			return Removed.contains(Node.NodeId);
		});
		std::vector<FGuid> Affected(NodeIds.begin(), NodeIds.end());
		return CommitSemanticChange(Material, std::move(Candidate), std::move(Presentation),
			"Delete Material Nodes", std::move(Affected), {}, Transactions);
	}

	auto FMaterialGraphOperations::Connect(
		DMaterial& Material,
		const FMaterialGraphConnectRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (!FindNode(Candidate, Request.SourceNodeId))
			return MakeRejected("The material graph source node does not exist.");
		FMaterialProgramNode* Destination = FindNode(Candidate, Request.DestinationNodeId);
		if (!Destination)
			return MakeRejected("The material graph destination node does not exist.");
		if (Request.DestinationInputIndex >= Destination->Inputs.size())
			return MakeRejected("The material graph destination input does not exist.");
		const FMaterialProgramLink Link{Request.SourceNodeId, Request.SourceOutputIndex};
		if (Destination->Inputs[Request.DestinationInputIndex] == Link)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (!Request.bReplaceExisting)
			return MakeRejected("The material graph destination input is already connected.");
		Destination->Inputs[Request.DestinationInputIndex] = Link;
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Connect Material Nodes",
			{Request.SourceNodeId, Request.DestinationNodeId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::DisconnectInput(
		DMaterial& Material,
		const FGuid& DestinationNodeId,
		uint32 DestinationInputIndex,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		FMaterialProgramNode* Destination = FindNode(Candidate, DestinationNodeId);
		if (!Destination)
			return MakeRejected("The material graph destination node does not exist.");
		if (DestinationInputIndex >= Destination->Inputs.size())
			return MakeRejected("The material graph destination input does not exist.");
		Destination->Inputs.erase(Destination->Inputs.begin() + DestinationInputIndex);
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Disconnect Material Input", {DestinationNodeId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::AssignSurfaceOutput(
		DMaterial& Material,
		const FMaterialGraphSurfaceOutputRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (!FindNode(Candidate, Request.SourceNodeId))
			return MakeRejected("The material graph source node does not exist.");
		FMaterialProgramLink* Output = &GetMaterialSurfaceOutputLink(
			Candidate.Outputs, Request.Output);
		const FMaterialProgramLink Link{Request.SourceNodeId, Request.SourceOutputIndex};
		if (*Output == Link) return {.Status = EMaterialGraphCommandStatus::NoChange};
		*Output = Link;
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Assign Material Surface Output", {Request.SourceNodeId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::AssignAggregateSurface(
		DMaterial& Material, const FGuid& SourceNodeId,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		const FMaterialProgram* Current = Material.GetMaterialProgram();
		if (!Current) return MakeRejected("The material has no authored program.");
		const FMaterialProgramNode* Source = FindNode(*Current, SourceNodeId);
		if (!Source || Source->ResultType != EMaterialProgramValueType::Surface)
			return MakeRejected("Aggregate Material Output requires a Surface node.");
		FMaterialProgram Candidate = *Current;
		for (EMaterialSurfaceOutput Output : {
			EMaterialSurfaceOutput::BaseColor, EMaterialSurfaceOutput::Normal,
			EMaterialSurfaceOutput::Metallic, EMaterialSurfaceOutput::Roughness,
			EMaterialSurfaceOutput::AmbientOcclusion, EMaterialSurfaceOutput::Emissive,
			EMaterialSurfaceOutput::Opacity, EMaterialSurfaceOutput::OpacityMask})
			GetMaterialSurfaceOutputLink(Candidate.Outputs, Output) = {};
		Candidate.Outputs.Surface = {.SourceNodeId = SourceNodeId};
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Connect Aggregate Surface", {SourceNodeId}, {}, Transactions);
	}

	auto FMaterialGraphOperations::DisconnectAggregateSurface(
		DMaterial& Material, DTransactor* Transactions)
		-> FMaterialGraphCommandResult
	{
		const FMaterialProgram* Current = Material.GetMaterialProgram();
		if (!Current) return MakeRejected("The material has no authored program.");
		if (!Current->Outputs.Surface.SourceNodeId.IsValid())
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		FMaterialProgram Candidate = *Current;
		const FGuid Source = Candidate.Outputs.Surface.SourceNodeId;
		Candidate.Outputs.Surface = {};
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Disconnect Aggregate Surface", {Source}, {}, Transactions);
	}

	auto FMaterialGraphOperations::DisconnectSurfaceOutput(
		DMaterial& Material,
		EMaterialSurfaceOutput Output,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(
			Candidate.Outputs, Output);
		if (!Link.SourceNodeId.IsValid())
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		const FGuid Affected = Link.SourceNodeId;
		Link = {};
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Disconnect Material Surface Output", {Affected}, {}, Transactions);
	}

	auto FMaterialGraphOperations::SetSurfaceDefault(
		DMaterial& Material,
		const FMaterialGraphSurfaceDefaultRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		FMaterialProgramLiteral& Value = GetMaterialSurfaceOutputDefault(
			Candidate.Outputs, Request.Output);
		if (Value == Request.Value)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		Value = Request.Value;
		return CommitSemanticChange(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(),
			"Edit Material Surface Default", {}, {}, Transactions);
	}

	auto FMaterialGraphOperations::ResetSurfaceDefault(
		DMaterial& Material,
		EMaterialSurfaceOutput Output,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return SetSurfaceDefault(Material, {
			.Output = Output,
			.Value = GetMaterialSurfaceOutputDefault(
				MakeDefaultMaterialProgram().Outputs, Output)}, Transactions);
	}

	auto FMaterialGraphOperations::SetParameterValue(
		DMaterial& Material,
		const FGuid& ParameterId,
		FMaterialParameterValue Value,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		const std::vector Dependencies = InspectMaterialParameterDependencies(
			*Material.GetMaterialProgram(), Material.GetParameterDefinitions());
		if (std::ranges::none_of(Dependencies, [&](const auto& Dependency) {
			return Dependency.ParameterId == ParameterId;
		}))
			return MakeRejected(
				"Only a reachable material graph parameter can be edited here.");
		FResolvedMaterialParameter Resolved;
		if (!Material.ResolveParameterValue(ParameterId, Resolved)
			|| !Resolved.Definition)
			return MakeRejected("The material parameter definition is unavailable.");
		const FMaterialParameterValue BeforeValue = Resolved.Value;
		if (BeforeValue == Value)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (!Material.SetParameterValue(ParameterId, Value))
			return MakeRejected("The material rejected the parameter value.");
		if (Transactions)
		{
			const auto bRecorded = Transactions->CommitApplied(
				std::make_unique<FMaterialGraphParameterTransaction>(
					Material, ParameterId, BeforeValue,
					std::move(Value)));
			check(bRecorded);
		}
		std::vector<FGuid> AffectedNodes;
		for (const FMaterialProgramNode& Node : Material.GetMaterialProgram()->Nodes)
			if (Node.ParameterId == ParameterId) AffectedNodes.push_back(Node.Id);
		return {.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(AffectedNodes)};
	}

	auto FMaterialGraphOperations::PromoteSurfaceOutputToParameter(
		DMaterial& Material,
		const FMaterialGraphSurfaceNodeRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (static_cast<uint8>(Request.Output)
			> static_cast<uint8>(EMaterialSurfaceOutput::OpacityMask))
			return MakeRejected("The material surface output is invalid.");
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transactor is busy.");
		const FMaterialProgram BeforeProgram = *Material.GetMaterialProgram();
		if (GetMaterialSurfaceOutputLink(
			BeforeProgram.Outputs, Request.Output).SourceNodeId.IsValid())
			return MakeRejected(
				"Only an unconnected material surface output can be promoted.");
		if (BeforeProgram.Nodes.size() >= MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph node limit has been reached.");
		const FGuid ParameterId = GetMaterialSurfaceParameterId(Request.Output,
			MaterialParameters::EMaterialBuiltinParameterKind::Value);
		FResolvedMaterialParameter BeforeResolved;
		if (!Material.ResolveParameterValue(ParameterId, BeforeResolved))
			return MakeRejected(
				"The material surface parameter definition is unavailable.");
		const EMaterialProgramValueType Type =
			GetMaterialSurfaceOutputType(Request.Output);
		const FMaterialParameterValue AfterValue = MakeParameterValue(
			Type, GetMaterialSurfaceOutputDefault(
				BeforeProgram.Outputs, Request.Output));

		FMaterialProgram Candidate = BeforeProgram;
		FMaterialProgramNode Node;
		Node.Id = FGuid::NewGuid();
		Node.Opcode = EMaterialProgramOpcode::Parameter;
		Node.ResultType = Type;
		Node.ParameterId = ParameterId;
		if (const FMaterialParameterDefinition* Definition =
			Material.FindParameterDefinition(ParameterId))
			Node.DisplayName = Definition->DisplayName;
		const FGuid NodeId = Node.Id;
		Candidate.Nodes.push_back(std::move(Node));
		GetMaterialSurfaceOutputLink(Candidate.Outputs, Request.Output) =
			{NodeId, 0};
		FMaterialGraphPresentation CandidatePresentation =
			Material.GetMaterialGraphPresentation();
		CandidatePresentation.Nodes.push_back(
			{NodeId, Request.X, Request.Y});
		const FMaterialGraphPresentation BeforePresentation =
			Material.GetMaterialGraphPresentation();
		FMaterialGraphCommandResult Result = CommitSemanticChange(Material, Candidate,
			CandidatePresentation, "Promote Material Surface Parameter",
			{NodeId}, {NodeId}, nullptr);
		if (!Result) return Result;
		if (!Material.SetParameterValue(ParameterId, AfterValue))
		{
			FMaterialProgramValidationResult RollbackValidation;
			Material.SetMaterialProgram(BeforeProgram, RollbackValidation);
			Material.SetMaterialGraphPresentation(BeforePresentation);
			return MakeRejected(
				"The promoted material parameter value could not be initialized.");
		}
		if (Transactions)
		{
			const auto bRecorded = Transactions->CommitApplied(
				std::make_unique<FMaterialGraphSemanticTransaction>(Material,
					BeforeProgram, BeforePresentation, Candidate,
					CandidatePresentation,
					"Promote Material Surface Parameter", ParameterId,
					BeforeResolved.Value, AfterValue));
			check(bRecorded);
		}
		return Result;
	}

	auto FMaterialGraphOperations::AddTextureToSurfaceOutput(
		DMaterial& Material,
		const FMaterialGraphSurfaceNodeRequest& Request,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (static_cast<uint8>(Request.Output)
			> static_cast<uint8>(EMaterialSurfaceOutput::OpacityMask))
			return MakeRejected("The material surface output is invalid.");
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		const bool bNormal = Request.Output == EMaterialSurfaceOutput::Normal;
		const bool bVector = GetMaterialSurfaceOutputType(Request.Output)
			== EMaterialProgramValueType::Float3;
		const size_t RequiredNodeCount = bNormal ? 5u : (bVector ? 4u : 4u);
		if (Candidate.Nodes.size() + RequiredNodeCount
			> MaterialProgramMaxNodeCount)
			return MakeRejected(
				"Adding the texture branch would exceed the material graph node limit.");
		const FGuid TextureRole = GetMaterialSurfaceParameterId(Request.Output,
			MaterialParameters::EMaterialBuiltinParameterKind::Texture);
		FMaterialGraphPresentation Presentation =
			Material.GetMaterialGraphPresentation();
		std::vector<FGuid> Generated;
		auto AddNode = [&](EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType Type,
			std::vector<FMaterialProgramLink> Inputs,
			int32 X, int32 Y) -> FMaterialProgramNode& {
			FMaterialProgramNode Node;
			Node.Id = FGuid::NewGuid();
			Node.Opcode = Opcode;
			Node.ResultType = Type;
			Node.Inputs = std::move(Inputs);
			const FGuid Id = Node.Id;
			Candidate.Nodes.push_back(std::move(Node));
			Presentation.Nodes.push_back({Id, X, Y});
			Generated.push_back(Id);
			return Candidate.Nodes.back();
		};
		FMaterialProgramNode& Texture = AddNode(
			EMaterialProgramOpcode::TextureParameter,
			EMaterialProgramValueType::Texture2D, {},
			Request.X - 640, Request.Y - 80);
		Texture.ParameterId = TextureRole;
		if (const FMaterialParameterDefinition* Definition =
			Material.FindParameterDefinition(TextureRole))
			Texture.DisplayName = Definition->DisplayName;
		const FGuid TextureId = Texture.Id;
		FMaterialProgramNode& UV = AddNode(
			EMaterialProgramOpcode::TextureCoordinate,
			EMaterialProgramValueType::Float2, {},
			Request.X - 640, Request.Y + 80);
		UV.ParameterId = TextureRole;
		const FGuid UVId = UV.Id;
		FMaterialProgramNode& Sample = AddNode(
			EMaterialProgramOpcode::TextureSample2D,
			EMaterialProgramValueType::Float4,
			{{TextureId, 0}, {UVId, 0}}, Request.X - 320, Request.Y);
		FGuid ResultId = Sample.Id;

		FMaterialProgramNode& Swizzle = AddNode(
			EMaterialProgramOpcode::Swizzle,
			bNormal ? EMaterialProgramValueType::Float2
				: GetMaterialSurfaceOutputType(Request.Output),
			{{Sample.Id, 0}}, Request.X, Request.Y);
		if (bNormal)
		{
			Swizzle.SwizzleLength = 2;
			Swizzle.SwizzleX = 0;
			Swizzle.SwizzleY = 1;
		}
		else if (bVector)
		{
			Swizzle.SwizzleLength = 3;
			Swizzle.SwizzleX = 0;
			Swizzle.SwizzleY = 1;
			Swizzle.SwizzleZ = 2;
		}
		else
		{
			constexpr std::array<uint8, 8> Components{0, 0, 2, 1, 0, 0, 3, 0};
			Swizzle.SwizzleLength = 1;
			Swizzle.SwizzleX = Components[static_cast<size_t>(Request.Output)];
		}
		ResultId = Swizzle.Id;
		if (bNormal)
		{
			FMaterialProgramNode& Decode = AddNode(
				EMaterialProgramOpcode::DecodeNormalRG,
				EMaterialProgramValueType::Float3,
				{{Swizzle.Id, 0}}, Request.X + 320, Request.Y);
			ResultId = Decode.Id;
		}
		GetMaterialSurfaceOutputLink(Candidate.Outputs, Request.Output) =
			{ResultId, 0};
		return CommitSemanticChange(Material, std::move(Candidate), std::move(Presentation),
			"Add Material Surface Texture", Generated, Generated,
			Transactions);
	}

	auto FMaterialGraphOperations::MoveNodes(
		DMaterial& Material,
		std::span<const FMaterialGraphNodePresentation> Positions,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Positions.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Positions.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph move request exceeds the node bound.");
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		std::vector<FGuid> Affected;
		std::unordered_set<FGuid> RequestedNodes;
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			if (!FindNode(*Material.GetMaterialProgram(), Position.NodeId))
				return MakeRejected("A moved material graph node does not exist.");
			if (!RequestedNodes.insert(Position.NodeId).second)
				return MakeRejected("A material graph move request contains a duplicate node GUID.");
			if (Position.X < -MaterialGraphPresentationCoordinateLimit
				|| Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit
				|| Position.Y > MaterialGraphPresentationCoordinateLimit)
				return MakeRejected("A material graph position is outside the supported coordinate range.");
			auto It = std::ranges::find(Presentation.Nodes, Position.NodeId,
				&FMaterialGraphNodePresentation::NodeId);
			if (It == Presentation.Nodes.end()) Presentation.Nodes.push_back(Position);
			else *It = Position;
			Affected.push_back(Position.NodeId);
		}
		return CommitPresentationChange(Material, std::move(Presentation),
			"Move Material Nodes", std::move(Affected), Transactions);
	}

	auto FMaterialGraphOperations::MoveMaterialOutput(
		DMaterial& Material,
		int32 X,
		int32 Y,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (X < -MaterialGraphPresentationCoordinateLimit
			|| X > MaterialGraphPresentationCoordinateLimit
			|| Y < -MaterialGraphPresentationCoordinateLimit
			|| Y > MaterialGraphPresentationCoordinateLimit)
			return MakeRejected(
				"The Material Output position is outside the supported coordinate range.");
		FMaterialGraphPresentation Presentation =
			Material.GetMaterialGraphPresentation();
		Presentation.bHasMaterialOutputPosition = true;
		Presentation.MaterialOutputX = X;
		Presentation.MaterialOutputY = Y;
		return CommitPresentationChange(Material, std::move(Presentation),
			"Move Material Output", {}, Transactions);
	}

	auto FMaterialGraphOperations::CalculateLayout(
		const DMaterial& Material,
		std::span<const FGuid> NodeIds,
		FMaterialGraphPresentation& OutPresentation) -> FMaterialGraphCommandResult
	{
		OutPresentation = Material.GetMaterialGraphPresentation();
		const FMaterialProgram& Program = *Material.GetMaterialProgram();
		if (NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph layout request exceeds the node bound.");
		std::unordered_set<FGuid> Requested(NodeIds.begin(), NodeIds.end());
		if (NodeIds.empty())
			for (const FMaterialProgramNode& Node : Program.Nodes)
				Requested.insert(Node.Id);
		if (Requested.size() != (NodeIds.empty() ? Program.Nodes.size() : NodeIds.size()))
			return MakeRejected("The material graph layout request contains duplicate node GUIDs.");
		for (const FGuid& Id : Requested)
			if (!FindNode(Program, Id))
				return MakeRejected("A material graph layout node does not exist.");

		std::unordered_map<FGuid, std::vector<FGuid>> Consumers;
		for (const FMaterialProgramNode& Node : Program.Nodes)
			for (const FMaterialProgramLink& Input : Node.Inputs)
				Consumers[Input.SourceNodeId].push_back(Node.Id);
		std::unordered_set<FGuid> SurfaceSources{
			Program.Outputs.BaseColor.SourceNodeId,
			Program.Outputs.Normal.SourceNodeId,
			Program.Outputs.Metallic.SourceNodeId,
			Program.Outputs.Roughness.SourceNodeId,
			Program.Outputs.AmbientOcclusion.SourceNodeId,
			Program.Outputs.Emissive.SourceNodeId,
			Program.Outputs.Opacity.SourceNodeId,
			Program.Outputs.OpacityMask.SourceNodeId,
			Program.Outputs.Surface.SourceNodeId,
		};
		std::unordered_map<FGuid, uint32> DistanceToSink;
		std::function<uint32(const FGuid&)> Visit = [&](const FGuid& Id) -> uint32 {
			if (const auto It = DistanceToSink.find(Id); It != DistanceToSink.end())
				return It->second;
			uint32 Distance = SurfaceSources.contains(Id) ? 1u : 0u;
			if (const auto It = Consumers.find(Id); It != Consumers.end())
				for (const FGuid& Consumer : It->second)
					Distance = std::max(Distance, Visit(Consumer) + 1);
			DistanceToSink.emplace(Id, Distance);
			return Distance;
		};
		uint32 MaximumDistance = 0;
		for (const FMaterialProgramNode& Node : Program.Nodes)
			MaximumDistance = std::max(MaximumDistance, Visit(Node.Id));

		std::map<uint32, std::vector<FGuid>> Columns;
		for (const FGuid& Id : Requested)
			Columns[MaximumDistance - DistanceToSink[Id]].push_back(Id);
		std::unordered_map<FGuid, uint32> NodeColumns;
		std::unordered_map<FGuid, size_t> Ranks;
		for (auto& [Column, Nodes] : Columns)
		{
			std::ranges::sort(Nodes);
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
			{
				NodeColumns.emplace(Nodes[Index], Column);
				Ranks[Nodes[Index]] = Index;
			}
		}

		const std::array SurfaceOrder{
			Program.Outputs.BaseColor.SourceNodeId,
			Program.Outputs.Normal.SourceNodeId,
			Program.Outputs.Metallic.SourceNodeId,
			Program.Outputs.Roughness.SourceNodeId,
			Program.Outputs.AmbientOcclusion.SourceNodeId,
			Program.Outputs.Emissive.SourceNodeId,
			Program.Outputs.Opacity.SourceNodeId,
			Program.Outputs.OpacityMask.SourceNodeId,
			Program.Outputs.Surface.SourceNodeId,
		};
		std::unordered_map<FGuid, size_t> SurfaceRanks;
		for (size_t Index = 0; Index < SurfaceOrder.size(); ++Index)
			SurfaceRanks.try_emplace(SurfaceOrder[Index], Index);
		if (!Columns.empty())
		{
			auto& SinkNodes = Columns.rbegin()->second;
			std::ranges::stable_sort(SinkNodes, [&](const FGuid& A, const FGuid& B) {
				const size_t RankA = SurfaceRanks.contains(A)
					? SurfaceRanks[A] : SurfaceOrder.size();
				const size_t RankB = SurfaceRanks.contains(B)
					? SurfaceRanks[B] : SurfaceOrder.size();
				return RankA == RankB ? A < B : RankA < RankB;
			});
			for (size_t Index = 0; Index < SinkNodes.size(); ++Index)
				Ranks[SinkNodes[Index]] = Index;
		}

		auto Median = [&](const FGuid& Id, uint32 NeighborColumn,
			bool bUseInputs) -> float {
			std::vector<size_t> NeighborRanks;
			if (bUseInputs)
			{
				const FMaterialProgramNode* Node = FindNode(Program, Id);
				if (Node)
					for (const FMaterialProgramLink& Input : Node->Inputs)
						if (NodeColumns.contains(Input.SourceNodeId)
							&& NodeColumns[Input.SourceNodeId] == NeighborColumn)
							NeighborRanks.push_back(Ranks[Input.SourceNodeId]);
			}
			else if (const auto It = Consumers.find(Id); It != Consumers.end())
			{
				for (const FGuid& Consumer : It->second)
					if (NodeColumns.contains(Consumer)
						&& NodeColumns[Consumer] == NeighborColumn)
						NeighborRanks.push_back(Ranks[Consumer]);
			}
			if (NeighborRanks.empty()) return static_cast<float>(Ranks[Id]);
			std::ranges::sort(NeighborRanks);
			const size_t Middle = NeighborRanks.size() / 2;
			if (NeighborRanks.size() % 2) return static_cast<float>(NeighborRanks[Middle]);
			return (static_cast<float>(NeighborRanks[Middle - 1])
				+ static_cast<float>(NeighborRanks[Middle])) * 0.5f;
		};
		auto SortColumn = [&](uint32 Column, uint32 NeighborColumn,
			bool bUseInputs) {
			auto It = Columns.find(Column);
			if (It == Columns.end()) return;
			auto& Nodes = It->second;
			const auto PreviousRanks = Ranks;
			std::ranges::stable_sort(Nodes, [&](const FGuid& A, const FGuid& B) {
				const float MedianA = Median(A, NeighborColumn, bUseInputs);
				const float MedianB = Median(B, NeighborColumn, bUseInputs);
				if (MedianA != MedianB) return MedianA < MedianB;
				if (PreviousRanks.at(A) != PreviousRanks.at(B))
					return PreviousRanks.at(A) < PreviousRanks.at(B);
				return A < B;
			});
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
				Ranks[Nodes[Index]] = Index;
		};
		if (!Columns.empty())
			for (uint32 Sweep = 0; Sweep < 4; ++Sweep)
			{
				for (auto It = std::next(Columns.begin()); It != Columns.end(); ++It)
					SortColumn(It->first, std::prev(It)->first, true);
				for (auto It = Columns.rbegin(); It != Columns.rend(); ++It)
				{
					const auto Next = std::next(It);
					if (Next != Columns.rend()) SortColumn(Next->first, It->first, false);
				}
			}

		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		const FMaterialGraphCanvasMetrics& Metrics = FMaterialGraphGeometry::GetMetrics();
		struct FRect { float MinX; float MinY; float MaxX; float MaxY; };
		std::vector<FRect> Occupied;
		if (!NodeIds.empty())
			for (const FMaterialGraphNodePresentation& Existing : Presentation.Nodes)
				if (!Requested.contains(Existing.NodeId))
				{
					const FMaterialProgramNode* Node = FindNode(Program, Existing.NodeId);
					if (!Node) continue;
					Occupied.push_back({static_cast<float>(Existing.X), static_cast<float>(Existing.Y),
						Existing.X + Metrics.NodeWidth,
						Existing.Y + FMaterialGraphGeometry::GetNodeHeight(
							static_cast<uint32>(Node->Inputs.size()))});
				}
		std::vector<FMaterialGraphNodePresentation> Positions;
		for (auto& [Column, Nodes] : Columns)
		{
			float Y = 0.0f;
			for (const FGuid& Id : Nodes)
			{
				const FMaterialProgramNode* Node = FindNode(Program, Id);
				const float Height = FMaterialGraphGeometry::GetNodeHeight(
					Node ? static_cast<uint32>(Node->Inputs.size()) : 0u);
				const float X = Column * (Metrics.NodeWidth + Metrics.ColumnGap);
				for (uint32 Attempt = 0; Attempt <= MaterialProgramMaxNodeCount; ++Attempt)
				{
					const FRect Candidate{X, Y, X + Metrics.NodeWidth, Y + Height};
					const auto Collision = std::ranges::find_if(Occupied,
						[&](const FRect& Rect) {
							return Candidate.MinX < Rect.MaxX && Candidate.MaxX > Rect.MinX
								&& Candidate.MinY < Rect.MaxY && Candidate.MaxY > Rect.MinY;
						});
					if (Collision == Occupied.end()) break;
					Y = Collision->MaxY + Metrics.RowGap;
					if (Attempt == MaterialProgramMaxNodeCount)
						return MakeRejected("The selected material graph layout has no collision-free placement.");
				}
				Positions.push_back({Id, static_cast<int32>(std::round(X)),
					static_cast<int32>(std::round(Y))});
				Occupied.push_back({X, Y, X + Metrics.NodeWidth, Y + Height});
				Y += Height + Metrics.RowGap;
			}
		}
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			auto It = std::ranges::find(Presentation.Nodes, Position.NodeId,
				&FMaterialGraphNodePresentation::NodeId);
			if (It == Presentation.Nodes.end()) Presentation.Nodes.push_back(Position);
			else *It = Position;
		}
		if (NodeIds.empty())
		{
			bool bFound = false;
			float MaximumX = 0.0f;
			float MinimumY = 0.0f;
			float MaximumY = 0.0f;
			for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			{
				const FMaterialProgramNode* Node = FindNode(Program, Position.NodeId);
				if (!Node) continue;
				const float Y = static_cast<float>(Position.Y);
				const float Height = FMaterialGraphGeometry::GetNodeHeight(
					static_cast<uint32>(Node->Inputs.size()));
				MaximumX = std::max(MaximumX,
					static_cast<float>(Position.X) + Metrics.NodeWidth);
				if (!bFound)
				{
					MinimumY = Y;
					MaximumY = Y + Height;
					bFound = true;
				}
				else
				{
					MinimumY = std::min(MinimumY, Y);
					MaximumY = std::max(MaximumY, Y + Height);
				}
			}
			const float OutputHeight = Metrics.SurfaceHeaderHeight
				+ Metrics.PinRowHeight
					* (Program.Outputs.Surface.SourceNodeId.IsValid() ? 1.0f : 8.0f)
				+ Metrics.BodyPadding;
			Presentation.bHasMaterialOutputPosition = true;
			Presentation.MaterialOutputX = static_cast<int32>(std::round(
				MaximumX + Metrics.ColumnGap));
			Presentation.MaterialOutputY = static_cast<int32>(std::round(
				bFound ? (MinimumY + MaximumY - OutputHeight) * 0.5f : 0.0f));
		}
		std::vector<FGuid> Affected(Requested.begin(), Requested.end());
		OutPresentation = std::move(Presentation);
		std::ranges::sort(Affected);
		return {
			.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(Affected),
		};
	}

	auto FMaterialGraphOperations::Layout(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialGraphPresentation Presentation;
		FMaterialGraphCommandResult Calculated = CalculateLayout(
			Material, NodeIds, Presentation);
		if (!Calculated) return Calculated;
		return CommitPresentationChange(Material, std::move(Presentation),
			"Layout Material Graph", std::move(Calculated.AffectedNodeIds), Transactions);
	}

	auto FMaterialGraphOperations::CopySelection(
		const DMaterial& Material,
		std::span<const FGuid> NodeIds,
		FMaterialGraphClipboardPayload& OutPayload)
		-> FMaterialGraphCommandResult
	{
		OutPayload = {};
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph copy selection is empty or exceeds the node bound.");
		std::unordered_set<FGuid> Selected(NodeIds.begin(), NodeIds.end());
		if (Selected.size() != NodeIds.size())
			return MakeRejected("The material graph copy selection contains duplicate node GUIDs.");
		const FMaterialProgram& Program = *Material.GetMaterialProgram();
		const FMaterialGraphPresentation Presentation =
			SanitizeMaterialGraphPresentation(
				Material.GetMaterialGraphPresentation(), Program);
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			Positions.emplace(Position.NodeId, Position);
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit;
		int32 MinimumY = MaterialGraphPresentationCoordinateLimit;
		for (const FGuid& Id : Selected)
		{
			if (!FindNode(Program, Id))
				return MakeRejected("A copied material graph node does not exist.");
			const auto It = Positions.find(Id);
			if (It == Positions.end())
				return MakeRejected("A copied material graph node has no authored position.");
			MinimumX = std::min(MinimumX, It->second.X);
			MinimumY = std::min(MinimumY, It->second.Y);
		}
		std::vector<FGuid> Ordered(Selected.begin(), Selected.end());
		std::ranges::sort(Ordered);
		OutPayload.Nodes.reserve(Ordered.size());
		for (const FGuid& Id : Ordered)
		{
			FMaterialProgramNode Node = *FindNode(Program, Id);
			const FMaterialGraphNodePresentation& Position = Positions.at(Id);
			OutPayload.Nodes.push_back({
				.Node = std::move(Node),
				.RelativeX = Position.X - MinimumX,
				.RelativeY = Position.Y - MinimumY,
			});
		}
		if (Selected.contains(Program.Outputs.Surface.SourceNodeId))
		{
			OutPayload.bConnectAggregateSurface = true;
			OutPayload.AggregateSourceNodeId = Program.Outputs.Surface.SourceNodeId;
		}
		return {
			.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(Ordered),
		};
	}

	auto FMaterialGraphOperations::Paste(
		DMaterial& Material,
		const FMaterialGraphClipboardPayload& Payload,
		int32 X,
		int32 Y,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		if (Payload.SchemaVersion != CurrentMaterialGraphClipboardSchemaVersion)
			return MakeRejected("The material graph clipboard schema version is unsupported.");
		if (Payload.Nodes.empty() || Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph clipboard node count is outside the supported bound.");
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (Candidate.Nodes.size() + Payload.Nodes.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("Pasting would exceed the material graph node limit.");

		std::unordered_map<FGuid, FGuid> Remap;
		Remap.reserve(Payload.Nodes.size());
		for (const FMaterialGraphClipboardNode& ClipboardNode : Payload.Nodes)
		{
			if (!ClipboardNode.Node.Id.IsValid()
				|| !Remap.emplace(ClipboardNode.Node.Id, FGuid{}).second)
				return MakeRejected("The material graph clipboard contains an invalid or duplicate node GUID.");
			if (ClipboardNode.RelativeX < 0 || ClipboardNode.RelativeY < 0
				|| ClipboardNode.RelativeX > MaterialGraphPresentationCoordinateLimit * 2
				|| ClipboardNode.RelativeY > MaterialGraphPresentationCoordinateLimit * 2)
				return MakeRejected("The material graph clipboard contains an invalid relative position.");
		}
		std::unordered_set<FGuid> UsedIds;
		UsedIds.reserve(Candidate.Nodes.size() + Payload.Nodes.size());
		for (const FMaterialProgramNode& Node : Candidate.Nodes) UsedIds.insert(Node.Id);
		for (const FMaterialGraphClipboardNode& ClipboardNode : Payload.Nodes)
		{
			FGuid& NewId = Remap.at(ClipboardNode.Node.Id);
			do NewId = FGuid::NewGuid(); while (UsedIds.contains(NewId));
			UsedIds.insert(NewId);
		}
		if (Payload.bConnectAggregateSurface
			&& (!Payload.AggregateSourceNodeId.IsValid()
				|| !Remap.contains(Payload.AggregateSourceNodeId)))
			return MakeRejected("The aggregate Surface clipboard source is missing from the selection.");

		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		std::vector<FGuid> Generated;
		Generated.reserve(Payload.Nodes.size());
		for (const FMaterialGraphClipboardNode& ClipboardNode : Payload.Nodes)
		{
			FMaterialProgramNode Node = ClipboardNode.Node;
			Node.Id = Remap.at(ClipboardNode.Node.Id);
			for (FMaterialProgramLink& Input : Node.Inputs)
			{
				const auto It = Remap.find(Input.SourceNodeId);
				if (It != Remap.end()) Input.SourceNodeId = It->second;
				else if (!FindNode(Candidate, Input.SourceNodeId))
					return MakeRejected(
						"The material graph clipboard references an unavailable external input.");
			}
			const int64 PositionX = static_cast<int64>(X) + ClipboardNode.RelativeX;
			const int64 PositionY = static_cast<int64>(Y) + ClipboardNode.RelativeY;
			if (PositionX < -MaterialGraphPresentationCoordinateLimit
				|| PositionX > MaterialGraphPresentationCoordinateLimit
				|| PositionY < -MaterialGraphPresentationCoordinateLimit
				|| PositionY > MaterialGraphPresentationCoordinateLimit)
				return MakeRejected("Pasting would place a material graph node outside the supported coordinate range.");
			Generated.push_back(Node.Id);
			Presentation.Nodes.push_back({Node.Id,
				static_cast<int32>(PositionX), static_cast<int32>(PositionY)});
			Candidate.Nodes.push_back(std::move(Node));
		}
		if (Payload.bConnectAggregateSurface)
		{
			for (EMaterialSurfaceOutput Output : {
				EMaterialSurfaceOutput::BaseColor, EMaterialSurfaceOutput::Normal,
				EMaterialSurfaceOutput::Metallic, EMaterialSurfaceOutput::Roughness,
				EMaterialSurfaceOutput::AmbientOcclusion, EMaterialSurfaceOutput::Emissive,
				EMaterialSurfaceOutput::Opacity, EMaterialSurfaceOutput::OpacityMask})
				GetMaterialSurfaceOutputLink(Candidate.Outputs, Output) = {};
			Candidate.Outputs.Surface = {
				.SourceNodeId = Remap.at(Payload.AggregateSourceNodeId)};
		}
		return CommitSemanticChange(Material, std::move(Candidate), std::move(Presentation),
			"Paste Material Nodes", Generated, Generated, Transactions);
	}

	auto FMaterialGraphOperations::DuplicateNodes(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		int32 OffsetX,
		int32 OffsetY,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialGraphClipboardPayload Payload;
		FMaterialGraphCommandResult Copied = CopySelection(Material, NodeIds, Payload);
		if (!Copied) return Copied;
		const FMaterialGraphPresentation Presentation =
			SanitizeMaterialGraphPresentation(
				Material.GetMaterialGraphPresentation(), *Material.GetMaterialProgram());
		int32 MinimumX = MaterialGraphPresentationCoordinateLimit;
		int32 MinimumY = MaterialGraphPresentationCoordinateLimit;
		std::unordered_set<FGuid> Selected(NodeIds.begin(), NodeIds.end());
		for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			if (Selected.contains(Position.NodeId))
			{
				MinimumX = std::min(MinimumX, Position.X);
				MinimumY = std::min(MinimumY, Position.Y);
			}
		const int64 AnchorX = static_cast<int64>(MinimumX) + OffsetX;
		const int64 AnchorY = static_cast<int64>(MinimumY) + OffsetY;
		if (AnchorX < -MaterialGraphPresentationCoordinateLimit
			|| AnchorX > MaterialGraphPresentationCoordinateLimit
			|| AnchorY < -MaterialGraphPresentationCoordinateLimit
			|| AnchorY > MaterialGraphPresentationCoordinateLimit)
			return MakeRejected("Duplicating would place a material graph node outside the supported coordinate range.");
		return Paste(Material, Payload, static_cast<int32>(AnchorX),
			static_cast<int32>(AnchorY), Transactions);
	}

	auto FMaterialGraphOperations::CutSelection(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		FMaterialGraphClipboardPayload& OutPayload,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialGraphCommandResult Copied = CopySelection(
			Material, NodeIds, OutPayload);
		if (!Copied) return Copied;
		FMaterialGraphCommandResult Removed = RemoveNodes(
			Material, NodeIds, Transactions);
		if (!Removed) OutPayload = {};
		return Removed;
	}

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
				std::make_unique<FMaterialGraphPresentationTransaction>(
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
			*Material.GetMaterialProgram(), Material.GetParameterDefinitions());
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
				std::make_unique<FMaterialGraphParameterTransaction>(
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
