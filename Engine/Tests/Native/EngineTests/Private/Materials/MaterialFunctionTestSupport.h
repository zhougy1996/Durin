#pragma once

#include "TypedMaterialGraphTestFixture.h"
#include "MaterialTestSupport.h"
#include "ExplicitMaterialProgramTestFixture.h"
#include "StandardMaterialFunctionTestFixture.h"
#include "AssetForge/Builtins/ImportedSurfaceRecipe.h"
#include "AssetForge/Builtins/PBRSurfaceMaterial.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Asset/Testing.h"
#include "Asset/References.h"
#include "Asset/OfflinePreparation.h"
#include "Asset/Cook.h"
#include "AssetTools/IAssetTools.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeTestSupport.h"
#include "MaterialGraphDocument.h"
#include "MaterialFunctionPreview.h"
#include "MaterialEditorModule.h"
#include "Widgets/MMaterialFunctionEditor.h"
#include "Editor/WorkspaceManager.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"

namespace
{
	template<class TOwner> auto GetFunctionCalls(const TOwner& Owner) -> std::vector<const Durin::DMaterialExpressionFunctionCall*>
	{
		std::vector<const Durin::DMaterialExpressionFunctionCall*> Calls;
		for (const auto& Expression : Owner.GetExpressionCollection().Expressions)
			if (const auto* Call = Durin::Cast<Durin::DMaterialExpressionFunctionCall>(Expression.Get())) Calls.push_back(Call);
		return Calls;
	}

	struct FFunctionTestExpressions
	{
		Durin::FMaterialFunctionSignature Signature;
		std::vector<Durin::TStrongObjectPtr<Durin::DMaterialExpression>> Expressions;
		auto Pointers() const -> std::vector<Durin::DMaterialExpression*>
		{
			std::vector<Durin::DMaterialExpression*> Result;
			for (const auto& Expression : Expressions) Result.push_back(Expression.Get());
			return Result;
		}
		auto Validate() const -> Durin::FMaterialProgramValidationResult
		{ return Durin::FMaterialExpressionBuildContext::ValidateFunction(Pointers(), Signature); }
		auto Apply(Durin::DMaterialFunction& Function) const -> Durin::FMaterialProgramValidationResult
		{ return Function.SetFunctionExpressions(Signature, Pointers()); }
	};
	auto CaptureFunctionExpressions(const Durin::DMaterialFunction& Function) -> FFunctionTestExpressions
	{
		FFunctionTestExpressions Graph;
		Graph.Signature = Function.GetFunctionSignature();
		for (const auto& Expression : Function.GetExpressionCollection().Expressions)
			Graph.Expressions.emplace_back(Durin::DuplicateObject(Expression.Get(), nullptr, Durin::NAME_None));
		return Graph;
	}

	auto PublishRootFunction(Durin::DMaterial& Material, Durin::DMaterialFunction& Function,
		Durin::FGuid CallId) -> Durin::FMaterialProgramValidationResult
	{
		auto Call = Durin::Testing::MakeGraphExpression<Durin::DMaterialExpressionFunctionCall>(CallId);
		const auto& Output = Function.GetFunctionSignature().Outputs[0];
		Call->Function = &Function;
		Call->Outputs = {{Output.Id, Output.Type}};
		const std::array<Durin::DMaterialExpression*, 1> Expressions{Call.Get()};
		Durin::FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.Surface = {.ExpressionId = CallId, .OutputId = Output.Id};
		return Material.SetMaterialExpressions(Expressions, Outputs);
	}

	auto BuildTypedExpressions(std::span<Durin::DMaterialExpression* const> Expressions,
		const Durin::FMaterialExpressionSurfaceOutputs& Outputs) -> Durin::FMaterialExpressionBuildResult
	{
		Durin::FMaterialExpressionBuildEnvironment Environment;
		Environment.FindFunction = [](const Durin::DMaterialFunctionInterface& Function)
			-> std::optional<Durin::FMaterialExpressionFunctionBody> {
			if (const auto* Owner = Durin::Cast<Durin::DMaterialFunction>(&Function))
				return Owner->GetExpressionBody();
			return std::nullopt;
		};
		Durin::FMaterialExpressionBuildContext Context(Expressions, std::move(Environment));
		return Context.FinishSurface(Outputs);
	}

	auto NormalizeTypedExpressions(std::span<Durin::DMaterialExpression* const> Expressions,
		const Durin::FMaterialExpressionSurfaceOutputs& Outputs) -> Durin::FMaterialNormalizationResult
	{
		auto Built = BuildTypedExpressions(Expressions, Outputs);
		if (!Built) return {.Diagnostics = std::move(Built.Diagnostics)};
		Durin::FMaterialIRCompilerInput Input;
		Input.IR = std::move(Built.IR);
		Input.Parameters = std::move(Built.Parameters);
		Input.Sources = std::move(Built.Sources);
		Input.Environment.CompilerIdentity = "TypedFunctionExpressionTest";
		return Durin::NormalizeMaterialIR(Input);
	}
}

namespace
{
	auto FunctionPort(uint32 Id, Durin::EMaterialProgramValueType Type, std::string Name)
		-> Durin::FMaterialFunctionPort
	{
		return {.Id = {0xa04759c1, 1, 2, Id}, .Type = Type, .Name = std::move(Name)};
	}

	auto AddFunctionCall(Durin::DMaterialFunction& Caller, Durin::DMaterialFunctionInterface& Callee) -> void
	{
		using namespace Durin;
		std::vector<TStrongObjectPtr<DMaterialExpression>> Expressions;
		for (const auto& Expression : Caller.GetExpressionCollection().Expressions)
			Expressions.emplace_back(DuplicateObject(Expression.Get(), nullptr, NAME_None));
		const FGuid CallId{0x538d091e, 1, 2, static_cast<uint32>(Expressions.size() + 1)};
		const auto& Output = Callee.GetFunctionSignature().Outputs[0];
		auto Call = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>(CallId);
		Call->Function = &Callee; Call->Outputs = {{Output.Id, Output.Type}};
		auto* Terminal = Cast<DMaterialExpressionFunctionOutput>(Expressions[1].Get());
		ASSERT_NE(Terminal, nullptr);
		Terminal->Source = {CallId, 0, Output.Id};
		Expressions.emplace_back(Call.Get());
		std::vector<DMaterialExpression*> Values;
		for (const auto& Expression : Expressions) Values.push_back(Expression.Get());
		ASSERT_TRUE(Caller.SetFunctionExpressions(Caller.GetFunctionSignature(), Values));
	}
}
