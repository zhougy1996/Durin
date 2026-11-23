#include "Graph/MaterialGraphCatalog.h"
#include "MaterialGraphPinSource.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionTextureParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/Material.h"

#include "Graph/MaterialGraphValueTypes.h"
#include "Asset/AssetPicker.h"
#include "Misc/StringHelper.h"
#include "MaterialGraphExpressionRegistry.h"

namespace Durin::MaterialEditor
{
	auto MakeCreationAction(const FMaterialGraphCatalogEntry& Entry) -> FMaterialGraphCreationAction
	{
		return {.Id = std::format("expression:{}:{}", Entry.ExpressionClass ? Entry.ExpressionClass->GetName() : std::string{},
			IsMaterialAdaptiveNumeric(Entry.Opcode) ? 0u : static_cast<uint32>(Entry.ResultType)),
			.Name = Entry.OperationName, .Category = Entry.Category,
			.Keywords = std::string(GetProgramTypeName(Entry.ResultType)) + " " + Entry.Description,
			.Description = Entry.Description,
			.bFunction = Entry.Opcode != EMaterialProgramOpcode::Parameter
				&& Entry.Opcode != EMaterialProgramOpcode::TextureParameter
				&& Entry.Opcode != EMaterialProgramOpcode::TextureSampleParameter2D
				&& Entry.Opcode != EMaterialProgramOpcode::TextureSampleParameterCube, .Payload = Entry};
	}
	auto MakeFunctionCreationAction(std::string Path) -> FMaterialGraphCreationAction
	{
		return {.Id = "function:" + Path,
			.Name = AssetPicker::GetAssetPathDisplayName(Path, EAssetPathDisplayMode::AssetName),
			.Category = "Functions",
			.Keywords = "function call " + Path, .Payload = std::move(Path)};
	}
	auto MakeCollectionParameterCreationAction(std::string Path,
		FGuid ParameterId, std::string ParameterName)
		-> FMaterialGraphCreationAction
	{
		const std::string CollectionName = AssetPicker::GetAssetPathDisplayName(
			Path, EAssetPathDisplayMode::AssetName);
		return {
			.Id = std::format("collection:{}:{}", Path, ParameterId.ToString()),
			.Name = ParameterName,
			.Category = "Parameters",
			.Keywords = "collection parameter " + CollectionName + " "
				+ ParameterName,
			.Description = "Reads " + ParameterName + " from " + CollectionName
				+ " in the current world.",
			.Payload = FMaterialGraphCollectionParameterCreation{
				std::move(Path), ParameterId, std::move(ParameterName)},
		};
	}
	auto MakePortCreationAction(bool bOutput, EMaterialProgramValueType Type) -> FMaterialGraphCreationAction
	{
		return {.Id = std::format("port:{}:{}", bOutput ? "output" : "input", static_cast<uint32>(Type)), .Name = bOutput ? "Function Output" : "Function Input",
			.Category = "Functions", .Keywords = "function port", .bMaterial = false,
			.Payload = FMaterialGraphPortCreation{bOutput, Type}};
	}
	namespace
	{
		auto MakeCatalogEntry(const FMaterialGraphExpressionRegistration& Registration,
			const FMaterialExpressionAuthoringShape& Shape) -> FMaterialGraphCatalogEntry
		{
			const auto& Description = *Registration.Description;
			FMaterialGraphCatalogEntry Entry;
			Entry.ExpressionClass = Description.ExpressionClass;
			Entry.OperationName = Description.Name;
			Entry.Category = Registration.Category;
			Entry.Description = Description.Description;
			Entry.Opcode = *Description.SemanticOpcode;
			Entry.ResultType = Shape.ResultType;
			Entry.InputNames = Shape.InputNames;
			Entry.AcceptedInputTypes = Shape.AcceptedInputTypes;
			if (Entry.Opcode == EMaterialProgramOpcode::Parameter)
			{
				Entry.OperationName = Shape.ResultType == EMaterialProgramValueType::Float ? "Scalar Parameter" : "Vector Parameter";
				Entry.Description = "Create a new numeric parameter exposed to material instances.";
			}
			return Entry;
		}

		auto NormalizeSearchText(std::string_view Value) -> std::string
		{
			return StringUtils::FoldAscii(Value);
		}

		auto GetDescriptionSearchField(const FMaterialGraphCatalogEntry& Entry) -> std::string
		{
			const auto* Descriptor = FindMaterialExpressionDescription(Entry.ExpressionClass);
			const auto Keywords = Descriptor ? Descriptor->SearchKeywords : std::string_view{};
			return NormalizeSearchText(Keywords.empty() ? Entry.Description : Entry.Description + " " + std::string(Keywords));
		}

		auto PrepareSearchFields(FMaterialGraphCatalogEntry& Entry) -> void
		{
			Entry.NormalizedSearchFields = {
				NormalizeSearchText(Entry.OperationName),
				NormalizeSearchText(Entry.Category),
				GetDescriptionSearchField(Entry),
				NormalizeSearchText(GetProgramTypeName(Entry.ResultType)),
			};
		}

	}

	auto FMaterialGraphNodeDescriptor::GetConstantLiteral() const -> FMaterialProgramLiteral
	{
		const auto* Value = std::get_if<FMaterialParameterValue>(&Data);
		return Value ? ReadParameterLiteral(ResultType, *Value) : FMaterialProgramLiteral{};
	}

	auto DefaultNodePresentation(const FMaterialGraphNodeDescriptor& Node, size_t Ordinal) -> FMaterialGraphNodePresentation
	{
		return {.NodeId = Node.Id, .X = Node.bMaterialOutput ? 1280 : static_cast<int32>(Ordinal % 4) * 320,
			.Y = Node.bMaterialOutput ? 0 : static_cast<int32>(Ordinal / 4) * 240};
	}

	auto ReadGraphOutputs(const DMaterial& Material) -> FMaterialAttributeValues
	{
		FMaterialAttributeValues Result;
		Result.Attributes = ToGraphLink(*Material.GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes));
		for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
		{
			const auto Input = *Material.GetNumericInputForProperty(Definition.Property);
			GetMaterialSurfaceOutputLink(Result, Definition.Property) = ToGraphLink(Input.Connection);
			const auto V = ReadMaterialNumericDefault(Input, Definition.Property);
			GetMaterialSurfaceOutputDefault(Result, Definition.Property) = {V[0], V.size() > 1 ? V[1] : 0.f, V.size() > 2 ? V[2] : 0.f};
		}
		return Result;
	}

	auto ReadMaterialGraph(const DObject* Owner, FGuid RootViewId, std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::optional<std::span<const FGuid>> Nodes, const FMaterialGraphNodeVisitor& Visit) -> FMaterialGraphView
	{
		std::unordered_set<FGuid> Selected;
		if (Nodes) Selected.insert(Nodes->begin(), Nodes->end());
		const auto* Material = Cast<DMaterial>(Owner);
		const auto* Function = Cast<DMaterialFunction>(Owner);
		if (!Material && !Function) return {};
		const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions : Function->GetExpressionCollection().Expressions;
		FMaterialGraphView Result;
		std::unordered_map<const DClass*, std::vector<const FMaterialGraphCatalogEntry*>> BaseShapes;
		for (const auto& Entry : Catalog) BaseShapes[Entry.ExpressionClass].push_back(&Entry);
		for (size_t Ordinal = 0; Ordinal < Expressions.size(); ++Ordinal)
		{
			auto* Expression = Expressions[Ordinal].Get();
			if (Nodes && !Selected.contains(Expression->Id)) continue;
			FMaterialGraphNodeDescriptor Node{.Id = Expression->Id};
			const auto* Description = FindMaterialExpressionDescription(Expression->GetClass());
			if (Description)
			{
				if (Description->SemanticOpcode) Node.Opcode = *Description->SemanticOpcode;
				if (!Description->Shapes.empty()) Node.ResultType = Description->Shapes.front().ResultType;
			}
			if (const auto* Texture = Cast<DMaterialExpressionTextureParameter>(Expression); Texture && !IsMaterialSamplingNode(Node.Opcode))
				Node.ResultType = Texture->DefaultValue.Dimension == EMaterialTextureDimension::TextureCube ? EMaterialProgramValueType::TextureCube : EMaterialProgramValueType::Texture2D;
			if (auto* Type = Expression->GetClass()->FindPropertyByName("ResultType"))
				Node.ResultType = *static_cast<const EMaterialProgramValueType*>(Type->GetValuePtr(Expression));
			if (const auto* Constant = Cast<DMaterialExpressionScalarConstant>(Expression)) Node.Data = FMaterialParameterValue::MakeScalar(Constant->Value);
			else if (const auto* Constant = Cast<DMaterialExpressionVector2Constant>(Expression)) Node.Data = FMaterialParameterValue::MakeVector2(Constant->Value);
			else if (const auto* Constant = Cast<DMaterialExpressionVector3Constant>(Expression)) Node.Data = FMaterialParameterValue::MakeVector(Constant->Value);
			else if (const auto* Constant = Cast<DMaterialExpressionVector4Constant>(Expression)) Node.Data = FMaterialParameterValue::MakeVector4(Constant->Value);
			else if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameterCube>(Expression)) Node.Data = FMaterialGraphSampleInfo{Sample->Metadata.Id};
			else if (Cast<DMaterialExpressionTextureSampleCube>(Expression)) Node.Data = FMaterialGraphSampleInfo{};
			else if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression)) Node.Data = FMaterialGraphSampleInfo{Sample->Metadata.Id};
			else if (const auto* Sample = Cast<DMaterialExpressionTextureSample2D>(Expression)) Node.Data = FMaterialGraphSampleInfo{};
			else if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression)) Node.Data = FMaterialGraphParameterInfo{Parameter->Metadata.Id};
			else if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression))
			{
				Node.Data = Swizzle->Components;
				if (!Swizzle->Components.empty()) Node.ResultType = static_cast<EMaterialProgramValueType>(Swizzle->Components.size() - 1);
			}
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
			{
				Node.Opcode = EMaterialProgramOpcode::FunctionCall;
				if (!Call->FunctionOutputs.empty()) Node.ResultType = Call->FunctionOutputs.front().ExpectedType;
			}
			if (const auto* Collection = Cast<DMaterialExpressionCollectionParameter>(Expression); Collection && IsValid(Collection->Collection.Get()))
				if (const auto* Declaration = Collection->Collection->FindDeclaration(Collection->ParameterId))
					Node.ResultType = GetProgramType(Declaration->Type);
			const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression);
			const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression);
			if (Input || Output)
			{
				Node.Opcode = Input ? EMaterialProgramOpcode::FunctionInput : EMaterialProgramOpcode::FunctionOutput;
				Node.ResultType = Input ? Input->Port.Type : Output->Port.Type;
			}
			const auto& Layout = Expression->EditorLayout;
			FMaterialGraphNodeView View{.Node = Node, .Presentation = Layout.bAuthored
				? FMaterialGraphNodePresentation{Node.Id, Layout.X, Layout.Y, Layout.DisplayName}
				: DefaultNodePresentation(Node, Ordinal)};
			const FMaterialGraphCatalogEntry* Shape = nullptr;
			const auto ShapeIt = BaseShapes.find(Expression->GetClass());
			if (ShapeIt != BaseShapes.end())
				for (const auto* Candidate : ShapeIt->second)
					if (Candidate->ResultType == Node.ResultType) { Shape = Candidate; break; }
			std::optional<FMaterialGraphCatalogEntry> FallbackShape;
			if (!Shape)
				if (const auto* Registration = FindMaterialGraphExpressionRegistration(Expression->GetClass()))
					for (const auto& Candidate : Registration->Description->Shapes)
						if (Candidate.ResultType == Node.ResultType)
						{
							FallbackShape = MakeCatalogEntry(*Registration, Candidate);
							Shape = &*FallbackShape;
							break;
						}
			View.PrimaryLabel = Shape ? Shape->OperationName : Description ? std::string(Description->Name) : "Unknown";
			Visit(std::move(View), {Expression, Material, Shape});
		}
		if (Material && (!Nodes || Selected.contains(RootViewId)))
		{
			const auto [X, Y] = Material->GetRootPosition();
			FMaterialGraphNodeView Root{.Node = {.Id = RootViewId, .bMaterialOutput = true}, .PrimaryLabel = "Material Output",
				.Presentation = {RootViewId, X, Y}};
			Visit(std::move(Root), {nullptr, Material, nullptr});
		}
		if (Material) Result.Outputs = ReadGraphOutputs(*Material);
		Result.bFunction = Function != nullptr;
		return Result;
	}

	auto EnumerateCatalog()
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		std::vector<FMaterialGraphCatalogEntry> Result;
		for (const auto& Registration : GetMaterialGraphExpressionRegistrations())
		{
			if (Registration.CreationKind != EMaterialGraphCreationKind::Direct) continue;
			for (const auto& Shape : Registration.Description->Shapes)
				Result.push_back(MakeCatalogEntry(Registration, Shape));
		}
		std::ranges::stable_sort(Result, {}, &FMaterialGraphCatalogEntry::OperationName);
		for (FMaterialGraphCatalogEntry& Entry : Result) PrepareSearchFields(Entry);
		return Result;
	}

	auto SearchCatalog(
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType)
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		return SearchCatalog(EnumerateCatalog(), Query, SourceType);
	}

	auto SearchCatalog(
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

	auto SearchCatalogIndices(
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
			const auto* Descriptor = FindMaterialGraphExpressionRegistration(Entry.ExpressionClass);
			if (!Descriptor || Descriptor->CreationKind != EMaterialGraphCreationKind::Direct) continue;
			switch (Descriptor->PaletteShape)
			{
			case EMaterialGraphPaletteShape::InspectOnly: continue;
			case EMaterialGraphPaletteShape::Scalar:
				if (Entry.ResultType != EMaterialProgramValueType::Float) continue;
				break;
			case EMaterialGraphPaletteShape::ScalarOrVector4:
				if (Entry.ResultType != EMaterialProgramValueType::Float
					&& Entry.ResultType != EMaterialProgramValueType::Float4) continue;
				break;
			case EMaterialGraphPaletteShape::Float2:
				if (Entry.ResultType != EMaterialProgramValueType::Float2) continue;
				break;
			case EMaterialGraphPaletteShape::Adaptive:
			case EMaterialGraphPaletteShape::AdaptiveVector:
				if (Entry.ResultType != SourceType.value_or(
					Descriptor->PaletteShape == EMaterialGraphPaletteShape::AdaptiveVector
						? EMaterialProgramValueType::Float2 : EMaterialProgramValueType::Float)) continue;
				break;
			case EMaterialGraphPaletteShape::All: break;
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
					GetDescriptionSearchField(Entry),
					NormalizeSearchText(GetProgramTypeName(Entry.ResultType)),
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
			if (EntryA.ResultType != EntryB.ResultType)
				return EntryA.ResultType < EntryB.ResultType;
			return A.Ordinal < B.Ordinal;
		});
		std::vector<size_t> Result;
		Result.reserve(Ranked.size());
		for (const FRankedEntry& Entry : Ranked) Result.push_back(Entry.Index);
		return Result;
	}
}
