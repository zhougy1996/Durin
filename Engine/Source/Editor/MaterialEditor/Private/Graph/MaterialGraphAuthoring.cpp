#include "MaterialGraphAuthoring.h"

#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"
#include "Editor/Transaction.h"

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
			Entry.NodeTemplate.Opcode = Opcode;
			Entry.NodeTemplate.ResultType = ResultType;
			Entry.NodeTemplate.Inputs.resize(Inputs.size());
			Entry.AcceptedInputTypes = std::move(Inputs);
			return Entry;
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

		auto GetSurfaceOutputLink(
			FMaterialSurfaceOutputs& Outputs,
			EMaterialSurfaceOutput Output) -> FMaterialProgramLink*
		{
			switch (Output)
			{
			case EMaterialSurfaceOutput::BaseColor: return &Outputs.BaseColor;
			case EMaterialSurfaceOutput::Normal: return &Outputs.Normal;
			case EMaterialSurfaceOutput::Metallic: return &Outputs.Metallic;
			case EMaterialSurfaceOutput::Roughness: return &Outputs.Roughness;
			case EMaterialSurfaceOutput::AmbientOcclusion: return &Outputs.AmbientOcclusion;
			case EMaterialSurfaceOutput::Emissive: return &Outputs.Emissive;
			case EMaterialSurfaceOutput::Opacity: return &Outputs.Opacity;
			case EMaterialSurfaceOutput::OpacityMask: return &Outputs.OpacityMask;
			}
			return nullptr;
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

		class FMaterialGraphTransaction final : public ITransaction
		{
		public:
			FMaterialGraphTransaction(
				DMaterial& InMaterial,
				FMaterialProgram InBeforeProgram,
				FMaterialGraphPresentation InBeforePresentation,
				FMaterialProgram InAfterProgram,
				FMaterialGraphPresentation InAfterPresentation,
				bool bInSemantic,
				std::string InDescription)
				: Material(&InMaterial)
				, BeforeProgram(std::move(InBeforeProgram))
				, BeforePresentation(std::move(InBeforePresentation))
				, AfterProgram(std::move(InAfterProgram))
				, AfterPresentation(std::move(InAfterPresentation))
				, bSemantic(bInSemantic)
				, Description(std::move(InDescription))
			{
				AffectedPackages.front() = InMaterial.GetPackage();
			}

			auto GetDescription() const -> std::string_view override
			{
				return Description;
			}

			auto GetAffectedPackages() const -> std::span<DPackage* const> override
			{
				return AffectedPackages;
			}

			auto Undo() -> bool override
			{
				return Apply(BeforeProgram, BeforePresentation);
			}

			auto Redo() -> bool override
			{
				return Apply(AfterProgram, AfterPresentation);
			}

		private:
			auto Apply(
				const FMaterialProgram& Program,
				const FMaterialGraphPresentation& Presentation) -> bool
			{
				DMaterial* Target = Material.Get();
				if (!Target) return false;
				if (bSemantic)
				{
					FMaterialProgramValidationResult Validation;
					if (!Target->SetMaterialProgram(Program, Validation)) return false;
				}
				return Target->SetMaterialGraphPresentation(Presentation);
			}

			TWeakObjectPtr<DMaterial> Material;
			FMaterialProgram BeforeProgram;
			FMaterialGraphPresentation BeforePresentation;
			FMaterialProgram AfterProgram;
			FMaterialGraphPresentation AfterPresentation;
			bool bSemantic = false;
			std::string Description;
			std::array<DPackage*, 1> AffectedPackages{};
		};

		auto Commit(
			DMaterial& Material,
			FMaterialProgram CandidateProgram,
			FMaterialGraphPresentation CandidatePresentation,
			bool bSemantic,
			std::string Description,
			std::vector<FGuid> Affected,
			std::vector<FGuid> Generated,
			FTransactionManager* Transactions)
			-> FMaterialGraphCommandResult
		{
			if (!IsValid(&Material))
				return {.Status = EMaterialGraphCommandStatus::StaleOwner,
					.Message = "The material graph owner is no longer available."};
			if (Transactions && Transactions->HasPendingOperation())
				return MakeRejected("The editor transaction manager is busy.");
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

			if (bSemantic)
			{
				if (!Material.SetMaterialProgram(CandidateProgram, Validation))
					return MakeRejected("The material rejected the candidate program.",
						std::move(Validation.Diagnostics));
			}
			if (!Material.SetMaterialGraphPresentation(CandidatePresentation))
			{
				if (bSemantic)
				{
					FMaterialProgramValidationResult RollbackValidation;
					Material.SetMaterialProgram(BeforeProgram, RollbackValidation);
				}
				return MakeRejected("The material rejected the candidate graph presentation.");
			}

			if (Transactions)
			{
				const bool bRecorded = Transactions->CommitApplied(
					std::make_unique<FMaterialGraphTransaction>(
					Material,
					BeforeProgram,
					BeforePresentation,
					std::move(CandidateProgram),
					std::move(CandidatePresentation),
					bSemantic,
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
	}

	auto FMaterialGraphService::Inspect(const DMaterial& Material)
		-> FMaterialGraphView
	{
		FMaterialGraphView Result;
		const FMaterialProgram& Program = *Material.GetMaterialProgram();
		const FMaterialGraphPresentation Presentation =
			SanitizeMaterialGraphPresentation(
				Material.GetMaterialGraphPresentation(), Program);
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			Positions.emplace(Position.NodeId, Position);
		const std::vector<FMaterialGraphCatalogEntry> Catalog =
			EnumerateCatalog(Material);
		Result.Nodes.reserve(Program.Nodes.size());
		for (const FMaterialProgramNode& Node : Program.Nodes)
		{
			FMaterialGraphNodeView View{.Node = Node};
			const auto Shape = std::ranges::find_if(Catalog,
				[&](const FMaterialGraphCatalogEntry& Entry) {
					return Entry.NodeTemplate.Opcode == Node.Opcode
						&& Entry.NodeTemplate.ResultType == Node.ResultType
						&& (!Node.ParameterId.IsValid()
							|| Entry.NodeTemplate.ParameterId == Node.ParameterId);
				});
			View.Inputs.reserve(Node.Inputs.size());
			for (uint32 InputIndex = 0; InputIndex < Node.Inputs.size(); ++InputIndex)
			{
				const FMaterialProgramNode* Source = FindNode(
					Program, Node.Inputs[InputIndex].SourceNodeId);
				FMaterialGraphPinView Pin{
					.InputIndex = InputIndex,
					.Link = Node.Inputs[InputIndex],
					.SourceType = Source ? Source->ResultType
						: EMaterialProgramValueType::Float,
				};
				if (Shape != Catalog.end()
					&& InputIndex < Shape->AcceptedInputTypes.size())
					Pin.AcceptedTypes = Shape->AcceptedInputTypes[InputIndex];
				View.Inputs.push_back(std::move(Pin));
			}
			if (const auto It = Positions.find(Node.Id); It != Positions.end())
				View.Presentation = It->second;
			Result.Nodes.push_back(std::move(View));
		}
		std::ranges::sort(Result.Nodes, {}, [](const FMaterialGraphNodeView& View) {
			return View.Node.Id;
		});
		Result.Outputs = Program.Outputs;
		return Result;
	}

	auto FMaterialGraphService::EnumerateCatalog(const DMaterial& Material)
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
			Parameter.NodeTemplate.ParameterId = Definition.Id;
			Parameter.NodeTemplate.DisplayName = Definition.DisplayName;
			Result.push_back(std::move(Parameter));
			if (Type == EMaterialProgramValueType::Texture2D)
			{
				FMaterialGraphCatalogEntry Coordinates = MakeCatalogEntry(
					EMaterialProgramOpcode::TextureCoordinate,
					EMaterialProgramValueType::Float2);
				Coordinates.Name = std::format("{} UV", Definition.DisplayName);
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
		std::ranges::stable_sort(Result, {}, &FMaterialGraphCatalogEntry::Name);
		return Result;
	}

	auto FMaterialGraphService::CreateNode(
		DMaterial& Material,
		FMaterialGraphCreateNodeRequest Request,
		FTransactionManager* Transactions) -> FMaterialGraphCommandResult
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
		return Commit(Material, std::move(Candidate), std::move(Presentation), true,
			"Create Material Node", {GeneratedId}, {GeneratedId}, Transactions);
	}

	auto FMaterialGraphService::ReplaceNode(
		DMaterial& Material,
		FMaterialProgramNode Node,
		FTransactionManager* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		FMaterialProgramNode* Existing = FindNode(Candidate, Node.Id);
		if (!Existing) return MakeRejected("The material graph node does not exist.");
		const FGuid AffectedId = Node.Id;
		*Existing = std::move(Node);
		return Commit(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(), true,
			"Edit Material Node", {AffectedId}, {}, Transactions);
	}

	auto FMaterialGraphService::RemoveNodes(
		DMaterial& Material,
		std::span<const FGuid> NodeIds,
		FTransactionManager* Transactions) -> FMaterialGraphCommandResult
	{
		if (NodeIds.empty()) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph removal request exceeds the node bound.");
		std::unordered_set<FGuid> Removed(NodeIds.begin(), NodeIds.end());
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		const size_t BeforeCount = Candidate.Nodes.size();
		std::erase_if(Candidate.Nodes, [&](const FMaterialProgramNode& Node) {
			return Removed.contains(Node.Id);
		});
		if (Candidate.Nodes.size() == BeforeCount)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		for (FMaterialProgramNode& Node : Candidate.Nodes)
			std::erase_if(Node.Inputs, [&](const FMaterialProgramLink& Link) {
				return Removed.contains(Link.SourceNodeId);
			});
		FMaterialGraphPresentation Presentation = Material.GetMaterialGraphPresentation();
		std::erase_if(Presentation.Nodes, [&](const FMaterialGraphNodePresentation& Node) {
			return Removed.contains(Node.NodeId);
		});
		std::vector<FGuid> Affected(NodeIds.begin(), NodeIds.end());
		return Commit(Material, std::move(Candidate), std::move(Presentation), true,
			"Delete Material Nodes", std::move(Affected), {}, Transactions);
	}

	auto FMaterialGraphService::Connect(
		DMaterial& Material,
		const FMaterialGraphConnectRequest& Request,
		FTransactionManager* Transactions) -> FMaterialGraphCommandResult
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
		return Commit(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(), true,
			"Connect Material Nodes",
			{Request.SourceNodeId, Request.DestinationNodeId}, {}, Transactions);
	}

	auto FMaterialGraphService::AssignSurfaceOutput(
		DMaterial& Material,
		const FMaterialGraphSurfaceOutputRequest& Request,
		FTransactionManager* Transactions) -> FMaterialGraphCommandResult
	{
		FMaterialProgram Candidate = *Material.GetMaterialProgram();
		if (!FindNode(Candidate, Request.SourceNodeId))
			return MakeRejected("The material graph source node does not exist.");
		FMaterialProgramLink* Output = GetSurfaceOutputLink(Candidate.Outputs, Request.Output);
		if (!Output) return MakeRejected("The material surface output is invalid.");
		const FMaterialProgramLink Link{Request.SourceNodeId, Request.SourceOutputIndex};
		if (*Output == Link) return {.Status = EMaterialGraphCommandStatus::NoChange};
		*Output = Link;
		return Commit(Material, std::move(Candidate),
			Material.GetMaterialGraphPresentation(), true,
			"Assign Material Surface Output", {Request.SourceNodeId}, {}, Transactions);
	}

	auto FMaterialGraphService::MoveNodes(
		DMaterial& Material,
		std::span<const FMaterialGraphNodePresentation> Positions,
		FTransactionManager* Transactions) -> FMaterialGraphCommandResult
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
		return Commit(Material, *Material.GetMaterialProgram(), std::move(Presentation), false,
			"Move Material Nodes", std::move(Affected), {}, Transactions);
	}

	struct FMaterialGraphMoveSession::FImpl
	{
		TWeakObjectPtr<DMaterial> Material;
		FMaterialProgram Program;
		FMaterialGraphPresentation BeforePresentation;
		FMaterialGraphPresentation CurrentPresentation;
		std::unordered_set<FGuid> NodeIds;
		FTransactionManager* Transactions = nullptr;
		bool bActive = false;
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
		FTransactionManager* Transactions) -> FMaterialGraphCommandResult
	{
		if (Impl->bActive) return MakeRejected("A material graph move is already active.");
		if (!IsValid(&Material))
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		if (NodeIds.empty() || NodeIds.size() > MaterialProgramMaxNodeCount)
			return MakeRejected("The material graph move selection is empty or exceeds the node bound.");
		if (Transactions && Transactions->HasPendingOperation())
			return MakeRejected("The editor transaction manager is busy.");
		Impl->NodeIds.clear();
		for (const FGuid& Id : NodeIds)
		{
			if (!FindNode(*Material.GetMaterialProgram(), Id))
				return MakeRejected("A selected material graph node does not exist.");
			if (!Impl->NodeIds.insert(Id).second)
				return MakeRejected("The material graph move selection contains a duplicate node GUID.");
		}
		Impl->Material = &Material;
		Impl->Program = *Material.GetMaterialProgram();
		Impl->BeforePresentation = Material.GetMaterialGraphPresentation();
		Impl->CurrentPresentation = Impl->BeforePresentation;
		Impl->Transactions = Transactions;
		Impl->bActive = true;
		std::vector<FGuid> Affected(NodeIds.begin(), NodeIds.end());
		return {.Status = EMaterialGraphCommandStatus::Succeeded,
			.AffectedNodeIds = std::move(Affected)};
	}

	auto FMaterialGraphMoveSession::Apply(
		std::span<const FMaterialGraphNodePresentation> Positions)
		-> FMaterialGraphCommandResult
	{
		if (!Impl->bActive) return MakeRejected("No material graph move is active.");
		DMaterial* Material = Impl->Material.Get();
		if (!Material)
		{
			Impl->bActive = false;
			return {.Status = EMaterialGraphCommandStatus::StaleOwner,
				.Message = "The material graph owner is no longer available."};
		}
		for (const FMaterialGraphNodePresentation& Position : Positions)
			if (!Impl->NodeIds.contains(Position.NodeId))
				return MakeRejected("A material graph move preview addresses a node outside the selection.");
		FMaterialGraphCommandResult Result = FMaterialGraphService::MoveNodes(
			*Material, Positions, nullptr);
		if (Result) Impl->CurrentPresentation = Material->GetMaterialGraphPresentation();
		return Result;
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
			return MakeRejected("The editor transaction manager is busy.");
		const bool bChanged = Impl->BeforePresentation != Impl->CurrentPresentation;
		if (bChanged && Impl->Transactions)
		{
			const bool bRecorded = Impl->Transactions->CommitApplied(
				std::make_unique<FMaterialGraphTransaction>(
					*Material,
					Impl->Program,
					Impl->BeforePresentation,
					Impl->Program,
					Impl->CurrentPresentation,
					false,
					"Move Material Nodes"));
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
		Material->SetMaterialGraphPresentation(Impl->BeforePresentation);
		return {.Status = Impl->BeforePresentation == Impl->CurrentPresentation
			? EMaterialGraphCommandStatus::NoChange
			: EMaterialGraphCommandStatus::Succeeded};
	}

	auto FMaterialGraphMoveSession::IsActive() const -> bool
	{
		return Impl->bActive;
	}
}
