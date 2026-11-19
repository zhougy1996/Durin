#pragma once

#include "DerivedDataBuildExecutionPrivate.h"
#include "ShaderBuildSession.h"
#include "ShaderSharedOutput.h"
#include "Shader/ShaderCompiledOutput.h"
#include <gtest/gtest.h>

namespace Durin::Testing
{
	inline auto WriteShaderOutput(const FShaderCompileOptions& Options, const FShaderCompilerOutput& Product,
		DerivedData::IRequestOwner& Owner) -> DerivedData::FBuildOutput
	{
		using namespace DerivedData;
		const auto Descriptor = GetShaderBuildDescriptor();
		const auto Definition = FBuildDefinitionBuilder(Descriptor.Name).Build().value();
		const auto Action = FBuildActionBuilder(Definition, Descriptor).Build().value();
		FBuildOutputBuilder Builder(Descriptor.OutputType, Descriptor.OutputSchema,
			{.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes});
		auto Context = DerivedData::Private::FBuildExecutionAccess::MakeContext(Action, {}, Builder, Owner);
		ShaderSharedOutput::Write(Context, Options, Product);
		auto Output = std::move(Builder).Build().value();
		EXPECT_EQ(DerivedData::Private::FBuildExecutionAccess::GetExecutionFailure(Context).has_value(),
			!Output.GetObservations().empty());
		return Output;
	}

	inline auto WriteShaderOutput(const FShaderCompileOptions& Options, const FShaderCompilerOutput& Product) -> DerivedData::FBuildOutput
	{
		DerivedData::FRequestOwner Owner;
		return WriteShaderOutput(Options, Product, Owner);
	}
}
