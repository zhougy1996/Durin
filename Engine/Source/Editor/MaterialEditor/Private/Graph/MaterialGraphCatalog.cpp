#include "MaterialGraphDocument.h"

#include "Graph/MaterialGraphValueTypes.h"
#include "Misc/StringHelper.h"

namespace Durin::Editor::Material
{
	namespace
	{
		auto GetInputNames(EMaterialProgramOpcode Opcode, size_t Count)
			-> std::vector<std::string>
		{
			std::vector<std::string> Names;
			switch (Opcode)
			{
			case EMaterialProgramOpcode::UVChannel: Names = {"Channel"}; break;
			case EMaterialProgramOpcode::MakeSurface: Names = {"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"}; break;
			case EMaterialProgramOpcode::GetSurfaceAttributes:
			case EMaterialProgramOpcode::SetSurfaceAttributes: Names = {"Surface"}; break;
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
			case EMaterialProgramOpcode::UVChannel: return "Inputs";
			case EMaterialProgramOpcode::TextureSample2D:
			case EMaterialProgramOpcode::DecodeNormalRG:
			case EMaterialProgramOpcode::BlendNormalsRNM: return "Textures";
			case EMaterialProgramOpcode::MakeSurface:
			case EMaterialProgramOpcode::GetSurfaceAttributes:
			case EMaterialProgramOpcode::SetSurfaceAttributes: return "Surface";
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

		auto GetOpcodeName(EMaterialProgramOpcode Opcode) -> const char*
		{
			switch (Opcode)
			{
			case EMaterialProgramOpcode::Constant: return "Constant";
			case EMaterialProgramOpcode::Parameter: return "Parameter";
			case EMaterialProgramOpcode::TextureParameter: return "Texture Parameter";
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
			case EMaterialProgramOpcode::UVChannel: return "UV Channel";
			case EMaterialProgramOpcode::Sine: return "Sine";
			case EMaterialProgramOpcode::Cosine: return "Cosine";
			case EMaterialProgramOpcode::MakeSurface: return "Make Surface";
			case EMaterialProgramOpcode::FunctionInput: return "Function Input";
			case EMaterialProgramOpcode::FunctionOutput: return "Function Output";
			case EMaterialProgramOpcode::FunctionCall: return "Function Call";
			case EMaterialProgramOpcode::GetSurfaceAttributes: return "Get Surface Attributes";
			case EMaterialProgramOpcode::SetSurfaceAttributes: return "Set Surface Attributes";
			}
			return "Unknown";
		}

		auto MakeCatalogEntry(
			EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType ResultType,
			const FMaterialProgramNodeSignature& Signature)
			-> FMaterialGraphCatalogEntry
		{
			FMaterialGraphCatalogEntry Entry;
			Entry.OperationName = GetOpcodeName(Opcode);
			Entry.Category = GetCategory(Opcode);
			switch (Opcode)
			{
			case EMaterialProgramOpcode::Constant: Entry.Description = "A literal numeric value. Choose Float, Float2, Float3, or Float4 from the node type menu."; break;
			case EMaterialProgramOpcode::Parameter:
			case EMaterialProgramOpcode::TextureParameter: Entry.Description = "A value exposed by the material parameter definition."; break;
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
			case EMaterialProgramOpcode::UVChannel: Entry.Description = "Selects mesh UV channel 0-3 using an explicit scalar input, rounded and clamped."; break;
			case EMaterialProgramOpcode::Sine: Entry.Description = "Returns the component-wise sine in radians."; break;
			case EMaterialProgramOpcode::Cosine: Entry.Description = "Returns the component-wise cosine in radians."; break;
			case EMaterialProgramOpcode::MakeSurface: Entry.Description = "Combines eight explicit surface properties without hidden parameter access."; break;
			case EMaterialProgramOpcode::GetSurfaceAttributes: Entry.Description = "Reads selected attributes from a Surface."; break;
			case EMaterialProgramOpcode::SetSurfaceAttributes: Entry.Description = "Overrides selected attributes while retaining the base Surface."; break;
			}
			Entry.NodeTemplate.Opcode = Opcode;
			Entry.NodeTemplate.ResultType = ResultType;
			Entry.NodeTemplate.Inputs.resize(Signature.InputCount);
			Entry.InputNames = GetInputNames(Opcode, Signature.InputCount);
			for (uint8 Index = 0; Index < Signature.InputCount; ++Index)
				Entry.AcceptedInputTypes.emplace_back(
					Signature.Inputs[Index].begin(), Signature.Inputs[Index].end());
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
				NormalizeSearchText(Entry.Category),
				NormalizeSearchText(Entry.Description),
				NormalizeSearchText(GetProgramTypeName(Entry.NodeTemplate.ResultType)),
			};
		}

	}

	auto FMaterialGraphOperations::Inspect(const DMaterial& Material)
		-> FMaterialGraphView
	{
		const std::vector Catalog = EnumerateCatalog();
		return Inspect(Material, Catalog);
	}

	auto FMaterialGraphOperations::Inspect(
		const DMaterial& Material,
		std::span<const FMaterialGraphCatalogEntry> Catalog)
		-> FMaterialGraphView
	{		return FMaterialGraphDocument(const_cast<DMaterial&>(Material)).Inspect(Catalog);
	}

	auto FMaterialGraphDocument::Inspect() const -> FMaterialGraphView
	{
		return Inspect(FMaterialGraphOperations::EnumerateCatalog());
	}

	auto FMaterialGraphDocument::Inspect(std::span<const FMaterialGraphCatalogEntry> Catalog) const -> FMaterialGraphView
	{
		FMaterialGraphDocumentState State;
		if (!Capture(State)) return {};
		const auto BaseShapeKey = [](EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType ResultType) {
			return static_cast<uint32>(Opcode) << 8
				| static_cast<uint32>(ResultType);
		};

		FMaterialGraphView Result;
		const FMaterialProgram& Program = State.Program;
		const FMaterialGraphPresentation Presentation =
			SanitizeMaterialGraphPresentation(
				State.Presentation, Program);
		std::unordered_map<FGuid, const FMaterialProgramNode*> NodesById;
		NodesById.reserve(Program.Nodes.size());
		for (const FMaterialProgramNode& Node : Program.Nodes)
			NodesById.emplace(Node.Id, &Node);
		const auto FindCall = [&](const FGuid& NodeId) -> const FMaterialFunctionCall* {
			const auto Call = std::ranges::find(State.Calls, NodeId, &FMaterialFunctionCall::NodeId);
			return Call == State.Calls.end() ? nullptr : &*Call;
		};
		const auto SourceType = [&](const FMaterialProgramLink& Link) {
			if (const auto* Call = FindCall(Link.SourceNodeId))
			{
				const auto Output = std::ranges::find(Call->Outputs, Link.SourceOutputId, &FMaterialFunctionOutputBinding::OutputId);
				if (Output != Call->Outputs.end()) return Output->ExpectedType;
			}
			const auto Source = NodesById.find(Link.SourceNodeId);
			if (Source == NodesById.end()) return EMaterialProgramValueType::Float;
			if (Source->second->Opcode == EMaterialProgramOpcode::GetSurfaceAttributes && Link.SourceOutputIndex < 8)
				return GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Link.SourceOutputIndex));
			return Source->second->ResultType;
		};
		constexpr std::array AttributeNames{"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"};
		std::unordered_map<uint32, const FMaterialGraphCatalogEntry*> BaseShapes;
		BaseShapes.reserve(Catalog.size());
		for (const auto& Entry : Catalog)
			BaseShapes.emplace(BaseShapeKey(Entry.NodeTemplate.Opcode, Entry.NodeTemplate.ResultType), &Entry);
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		Positions.reserve(Presentation.Nodes.size());
		for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			Positions.emplace(Position.NodeId, Position);
		Result.Nodes.reserve(Program.Nodes.size());
		for (const FMaterialProgramNode& Node : Program.Nodes)
		{
			FMaterialGraphNodeView View{.Node = Node};
			const FMaterialGraphCatalogEntry* Shape = nullptr;
			const auto ShapeIt = BaseShapes.find(BaseShapeKey(Node.Opcode, Node.ResultType));
			if (ShapeIt != BaseShapes.end()) Shape = ShapeIt->second;
			View.PrimaryLabel = Shape
				? Shape->OperationName : GetOpcodeName(Node.Opcode);
			View.SecondaryLabel = Node.DisplayName;
			if (Node.ParameterId.IsValid())
				if (const auto Definition = std::ranges::find(State.Definitions, Node.ParameterId, &FMaterialParameterDefinition::Id); Definition != State.Definitions.end())
					View.SecondaryLabel = Definition->DisplayName;
			View.Inputs.reserve(Node.Inputs.size());
			for (uint32 InputIndex = 0; InputIndex < Node.Inputs.size(); ++InputIndex)
			{
				FMaterialGraphPinView Pin{
					.InputIndex = InputIndex,
					.Name = Shape
						&& InputIndex < Shape->InputNames.size()
						? Shape->InputNames[InputIndex] : "Value",
					.Link = Node.Inputs[InputIndex],
					.SourceType = SourceType(Node.Inputs[InputIndex]),
				};
				if (Shape
					&& InputIndex < Shape->AcceptedInputTypes.size())
					Pin.AcceptedTypes = Shape->AcceptedInputTypes[InputIndex];
				View.Inputs.push_back(std::move(Pin));
			}
			if (Node.Opcode == EMaterialProgramOpcode::FunctionCall)
			{
				if (const auto* Call = FindCall(Node.Id))
				{
					if (IsValid(Call->Function.Get()))
					{
						View.FunctionPath = Call->Function->GetObjectPath();
						View.SecondaryLabel = Call->Function->GetName();
						auto Inputs = Call->Function->GetFunctionSignature().Inputs;
						auto Outputs = Call->Function->GetFunctionSignature().Outputs;
						const auto Order = [](const auto& A, const auto& B) { return A.DisplayOrder != B.DisplayOrder ? A.DisplayOrder < B.DisplayOrder : A.Id < B.Id; };
						std::ranges::stable_sort(Inputs, Order);
						std::ranges::stable_sort(Outputs, Order);
						for (const auto& Port : Inputs)
						{
							const auto Binding = std::ranges::find(Call->Inputs, Port.Id, &FMaterialFunctionInputBinding::InputId);
							const auto Link = Binding == Call->Inputs.end() ? FMaterialProgramLink{} : Binding->Source;
							View.Inputs.push_back({.InputIndex = static_cast<uint32>(View.Inputs.size()), .Name = Port.Name,
								.Link = Link, .SourceType = Link.SourceNodeId.IsValid() ? SourceType(Link) : Port.Type,
								.AcceptedTypes = {Port.Type}, .PortId = Port.Id, .bRequired = Port.bRequired, .Default = Port.Default});
						}
						for (const auto& Port : Outputs) View.Outputs.push_back({.PortId = Port.Id, .Name = Port.Name, .Type = Port.Type});
					}
					else View.SecondaryLabel = "Missing function";
					for (const auto& Binding : Call->Inputs)
						if (std::ranges::none_of(View.Inputs, [&](const auto& Pin) { return Pin.PortId == Binding.InputId; }))
							View.Inputs.push_back({.InputIndex = static_cast<uint32>(View.Inputs.size()), .Name = "Missing input",
								.Link = Binding.Source, .SourceType = SourceType(Binding.Source), .AcceptedTypes = {Binding.ExpectedType},
								.PortId = Binding.InputId, .bMissing = true});
					for (const auto& Binding : Call->Outputs)
						if (std::ranges::none_of(View.Outputs, [&](const auto& Pin) { return Pin.PortId == Binding.OutputId; }))
							View.Outputs.push_back({.PortId = Binding.OutputId, .Name = "Missing output", .Type = Binding.ExpectedType, .bMissing = true});
				}
			}
			else if (Node.Opcode == EMaterialProgramOpcode::GetSurfaceAttributes)
			{
				for (uint8 Index = 0; Index < 8; ++Index)
					if (Node.SurfaceAttributeMask & (1u << Index)) View.Outputs.push_back({.OutputIndex = Index,
						.Name = AttributeNames[Index], .Type = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index))});
			}
			else if (Node.Opcode != EMaterialProgramOpcode::FunctionOutput)
				View.Outputs.push_back({.Name = GetProgramTypeName(Node.ResultType), .Type = Node.ResultType});
			if (Node.Opcode == EMaterialProgramOpcode::FunctionInput || Node.Opcode == EMaterialProgramOpcode::FunctionOutput)
			{
				const auto& Ports = Node.Opcode == EMaterialProgramOpcode::FunctionInput ? State.Signature.Inputs : State.Signature.Outputs;
				if (const auto Port = std::ranges::find(Ports, Node.FunctionPortId, &FMaterialFunctionPort::Id); Port != Ports.end())
				{
					View.SecondaryLabel = Port->Name;
					if (!View.Outputs.empty()) View.Outputs[0].Name = Port->Name;
					if (!View.Inputs.empty()) { View.Inputs[0].Name = Port->Name; View.Inputs[0].AcceptedTypes = {Port->Type}; }
				}
			}
			if ((Node.Opcode == EMaterialProgramOpcode::GetSurfaceAttributes
				|| Node.Opcode == EMaterialProgramOpcode::SetSurfaceAttributes) && !View.Inputs.empty())
			{
				View.Inputs[0].Name = "Surface";
				View.Inputs[0].AcceptedTypes = {EMaterialProgramValueType::Surface};
			}
			if (Node.Opcode == EMaterialProgramOpcode::SetSurfaceAttributes)
				for (const auto& Attribute : Node.SurfaceAttributes)
				{
					const auto Index = static_cast<uint32>(Attribute.Attribute);
					if (Index < 8) View.Inputs.push_back({.InputIndex = Index + 1, .Name = AttributeNames[Index],
						.Link = Attribute.Source, .SourceType = SourceType(Attribute.Source),
						.AcceptedTypes = {GetMaterialSurfaceOutputType(Attribute.Attribute)}});
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
		Result.bFunction = State.bFunction;
		check(State.bFunction || Presentation.bHasMaterialOutputPosition);
		Result.MaterialOutputPosition = {
			Presentation.MaterialOutputX,
			Presentation.MaterialOutputY};
		return Result;
	}

	auto FMaterialGraphOperations::EnumerateCatalog()
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		std::vector<FMaterialGraphCatalogEntry> Result;
		for (uint8 OpcodeValue = static_cast<uint8>(EMaterialProgramOpcode::Constant);
			OpcodeValue <= static_cast<uint8>(EMaterialProgramOpcode::SetSurfaceAttributes); ++OpcodeValue)
			for (uint8 TypeValue = static_cast<uint8>(EMaterialProgramValueType::Float);
				TypeValue <= static_cast<uint8>(EMaterialProgramValueType::Surface); ++TypeValue)
			{
				const auto Opcode = static_cast<EMaterialProgramOpcode>(OpcodeValue);
				const auto Type = static_cast<EMaterialProgramValueType>(TypeValue);
				const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
				if (!Signature) continue;
				auto Entry = MakeCatalogEntry(Opcode, Type, *Signature);
				if (Opcode == EMaterialProgramOpcode::GetSurfaceAttributes) Entry.NodeTemplate.SurfaceAttributeMask = 0xff;
				if (Opcode == EMaterialProgramOpcode::Parameter
					|| Opcode == EMaterialProgramOpcode::TextureParameter)
				{
					Entry.OperationName = Type == EMaterialProgramValueType::Float ? "Scalar Parameter"
						: Type == EMaterialProgramValueType::Float2 ? "Vector2 Parameter"
						: Type == EMaterialProgramValueType::Float3 ? "Vector3 Parameter"
						: Type == EMaterialProgramValueType::Float4 ? "Vector4 Parameter" : "Texture Parameter";
					Entry.Description = "Create a new parameter or reference an existing parameter of this type.";
				}
				if (Opcode == EMaterialProgramOpcode::Swizzle)
				{
					const uint8 Width = TypeValue + 1;
					Entry.NodeTemplate.SwizzleLength = Width;
					Entry.NodeTemplate.SwizzleX = 0;
					Entry.NodeTemplate.SwizzleY = std::min<uint8>(1, Width - 1);
					Entry.NodeTemplate.SwizzleZ = std::min<uint8>(2, Width - 1);
					Entry.NodeTemplate.SwizzleW = std::min<uint8>(3, Width - 1);
				}
				Result.push_back(std::move(Entry));
			}
		std::ranges::stable_sort(Result, {}, &FMaterialGraphCatalogEntry::OperationName);
		for (FMaterialGraphCatalogEntry& Entry : Result) PrepareSearchFields(Entry);
		return Result;
	}

	auto FMaterialGraphOperations::SearchCatalog(
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType)
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		return SearchCatalog(EnumerateCatalog(), Query, SourceType);
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
			// Keep dimensional shapes for inspection; the palette creates one scalar Constant.
			if (Entry.NodeTemplate.Opcode == EMaterialProgramOpcode::Constant
				&& Entry.NodeTemplate.ResultType != EMaterialProgramValueType::Float) continue;
			if (SourceType)
			{
				if (Entry.AcceptedInputTypes.empty()
					|| std::ranges::find(Entry.AcceptedInputTypes.front(), *SourceType)
						== Entry.AcceptedInputTypes.front().end()) continue;
			}
			uint8 Match = Needle.empty() ? 3 : 4;
			std::array<std::string, 4> FallbackSearchFields;
			const std::array<std::string, 4>* SearchFields =
				&Entry.NormalizedSearchFields;
			if (std::ranges::all_of(*SearchFields,
				[](const std::string& Field) { return Field.empty(); }))
			{
				FallbackSearchFields = {
					NormalizeSearchText(Entry.OperationName),
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
}
