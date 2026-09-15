#pragma once

#include "ExplicitMaterialProgramTestFixture.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "MaterialTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Texture/TextureFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "DObject/Package.h"
#include "NativeTestSupport.h"
#include "Texture/Texture2DCompilation.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Asset/AssetCompilingManager.h"

namespace
{
	auto SetExpandedProgram(Durin::DMaterial& Material) -> bool
	{
		return static_cast<bool>(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(Material));
	}

	// Slot binding and ownership migration need only color and opacity parameters.
	// Full PBR graph serialization remains covered by the expression-package case.
	auto SetBindingProgram(Durin::DMaterial& Material) -> bool
	{
		using namespace Durin;
		using Role = MaterialParameters::EMaterialBuiltinParameterRole;
		Testing::FTestMaterialExpressionGraph Graph;
		auto& Color = Graph.Add(EMaterialProgramOpcode::Parameter,
			EMaterialProgramValueType::Float3, {}, MaterialParameters::GetBuiltinParameterIds(Role::BaseColor).Value, {});
		auto& Opacity = Graph.Add(EMaterialProgramOpcode::Parameter,
			EMaterialProgramValueType::Float, {}, MaterialParameters::GetBuiltinParameterIds(Role::Opacity).Value, {});
		Graph.Outputs.BaseColor = Testing::MakeLink(Color);
		Graph.Outputs.Opacity = Testing::MakeLink(Opacity);
		return static_cast<bool>(Graph.Apply(Material));
	}

	auto RelocateAssetForTest(
		const Durin::FPackagePath& Source,
		const Durin::FPackagePath& Destination) -> Durin::FAssetResult
	{
		const Durin::FAssetRelocationMapping Mapping{Source, Destination};
		Durin::FAssetRelocationSummary Summary;
		Durin::FAssetMutationJob Transaction;
		Durin::FAssetResult Result =
			Durin::PrepareAssetRelocationJob(
				std::span{&Mapping, 1}, Summary, Transaction);
		if (Result) Result = Transaction.ResumeForward();
		return Result;
	}
}
