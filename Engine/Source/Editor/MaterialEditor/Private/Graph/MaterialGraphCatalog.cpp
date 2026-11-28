#include "Graph/MaterialGraphCatalog.h"

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
			Entry.Category = Description.Category;
			Entry.Description = Description.Description;
			Entry.Opcode = *Description.SemanticOpcode;
			Entry.ResultType = Shape.ResultType;
			Entry.InputNames = Shape.InputNames;
			Entry.AcceptedInputTypes = Shape.AcceptedInputTypes;
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
			case EMaterialGraphPaletteShape::Adaptive:
				if (Entry.ResultType != SourceType.value_or(Descriptor->Description->PaletteDefault.value_or(EMaterialProgramValueType::Float))) continue;
				break;
			case EMaterialGraphPaletteShape::All:
				if (Descriptor->Description->PaletteDefault && Entry.ResultType != *Descriptor->Description->PaletteDefault) continue;
				break;
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
