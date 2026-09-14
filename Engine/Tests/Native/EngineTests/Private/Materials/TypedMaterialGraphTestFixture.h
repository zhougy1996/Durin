#pragma once
#include "MaterialGraphDocument.h"
#include "DObject/Archive.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::Testing
{
	template<class T> auto MakeGraphExpression(FGuid Id = FGuid::NewGuid()) -> TStrongObjectPtr<T>
	{
		TStrongObjectPtr<T> Expression(NewObject<T>(nullptr, NAME_None));
		Expression->Id = Id;
		return Expression;
	}
	inline auto CreateGraphConstant(const Editor::Material::FMaterialGraphDocument& Document, float Value = 0.f,
		int32 X = 0, int32 Y = 0, DTransactor* Transactions = nullptr) -> Editor::Material::FMaterialGraphCommandResult
	{
		auto Constant = MakeGraphExpression<DMaterialExpressionScalarConstant>();
		Constant->Value = Value;
		return Document.CreateExpression(*Constant.Get(), X, Y, Transactions);
	}
	inline auto CreateGraphCatalogNode(const Editor::Material::FMaterialGraphDocument& Document,
		EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type = EMaterialProgramValueType::Float,
		FMaterialExpressionInput Input = {}, int32 X = 0, int32 Y = 0, DTransactor* Transactions = nullptr)
		-> Editor::Material::FMaterialGraphCommandResult
	{
		const auto Catalog = Editor::Material::FMaterialGraphOperations::EnumerateCatalog();
		const auto Entry = std::ranges::find_if(Catalog, [&](const auto& Value) { return Value.Opcode == Opcode && Value.ResultType == Type; });
		if (Entry == Catalog.end()) return {.Message = "Unknown test catalog shape."};
		return Document.CreateCatalogNode(*Entry, X, Y, Input, Transactions);
	}
}
