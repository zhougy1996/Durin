#pragma once

#include "ExplicitMaterialProgramTestFixture.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "MaterialTestSupport.h"
#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Materials/MaterialTypes.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"

#include <cstring>
#include <limits>

namespace
{
	auto MakeExpandedMaterial(const char* Name) -> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, Name);
		if (!Material || !Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material)) return nullptr;
		if (!FinishMaterialCompileForTest(*Material)) return nullptr;
		return Material;
	}

	auto MakeRenderFixture() -> Durin::FMaterialRenderRepresentation
	{
		using namespace Durin;
		const std::array Parameters{
			FMaterialCompilerParameterDeclaration{FGuid{1,0,0,1}, EMaterialParameterType::Vector},
			FMaterialCompilerParameterDeclaration{FGuid{2,0,0,1}, EMaterialParameterType::Scalar}};
		const auto Layout = CompileMaterialLayout(Parameters);
		FMaterialRenderRepresentationBuilder Builder(Layout.Layout);
		EXPECT_TRUE(Builder.SetVector(Parameters[0].Id, FVector3(0.5)));
		EXPECT_TRUE(Builder.SetScalar(Parameters[1].Id, 1.0f));
		FMaterialRenderRepresentation Result;
		FMaterialRenderValidationDiagnostic Diagnostic;
		EXPECT_TRUE(Builder.Build(Result, Diagnostic));
		return Result;
	}

	auto ReadFloat(Durin::FByteView Bytes, uint32 Offset) -> float
	{
		float Value = 0.0f;
		std::memcpy(&Value, Bytes.data() + Offset, sizeof(Value));
		return Value;
	}

	auto ReadParameterFloat(const Durin::FMaterialRenderData& Data,
		const Durin::FGuid& Id, uint32 Component = 0) -> float
	{
		const auto& Fields = Data.Representation.GetLayout().Fields;
		const auto It = std::ranges::find(
			Fields, Id, &Durin::FMaterialRenderField::ParameterId);
		EXPECT_NE(It, Fields.end());
		return It == Fields.end() ? 0.0f : ReadFloat(
			Data.Representation.GetUniformPayload(),
			It->Offset + Component * sizeof(float));
	}

}
