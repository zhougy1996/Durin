#pragma once

#if DURIN_WITH_EDITOR
#include "DerivedDataBuild.h"
#include "Logging/LogMacros.h"

namespace Durin::AssetDerivedDataBuild
{
	// Report once at execution, before the product crosses a publication boundary.
	// Misses are ordinary lookup outcomes; usable products never carry cache failures.
	template<typename TError, typename TFormat>
	auto ReportCacheIssues(const DerivedData::FBuildDefinition& Definition,
		const DerivedData::TBuildObservations<TError>& Observations, TFormat&& Format) -> void
	{
		DerivedData::VisitBuildIssues(Observations, [&](DerivedData::EBuildPhase Phase, const auto& Error) {
			std::string Detail;
			if constexpr (std::is_same_v<std::decay_t<decltype(Error)>, DerivedData::FCacheError>)
				Detail = Error.Diagnostic;
			else Detail = Format(Error);
			const auto Stage = Phase == DerivedData::EBuildPhase::Lookup ? "read"
				: Phase == DerivedData::EBuildPhase::Decode ? "decode"
				: Phase == DerivedData::EBuildPhase::Encode ? "encode" : "write";
			DURIN_WARN_CATEGORY("DerivedData", "{} cache {} [{}]: {}",
				Definition.GetFunction().Name, Stage, Definition.GetKey().ToString(), Detail.substr(0, 1600));
		});
	}
}
#endif
