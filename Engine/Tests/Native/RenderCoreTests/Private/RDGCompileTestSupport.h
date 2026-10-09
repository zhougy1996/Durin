#pragma once

#include "CoreMinimal.h"

#include "../../RDGTestAccess.h"
#include "RDG/RDG.h"

namespace Durin
{
	// Keeps the contract oracle and diagnostic baseline on the same 128-pass workload.
	inline auto PopulateRDGSyntheticCompileGraph(FRDGBuilder& Builder) -> void
	{
		static const auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Fixture", 512, 4, EBufferUsageFlags::UnorderedAccess));
		const auto Work = Builder.CreateBuffer({.Buffer = Buffer->GetDesc()}, "Fixture");
		for (uint32 Index = 0; Index < 128; ++Index)
		{
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder,
				"Pass" + std::to_string(Index), ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Work, 0, 512,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, Index == 0);
		}
	}
}
