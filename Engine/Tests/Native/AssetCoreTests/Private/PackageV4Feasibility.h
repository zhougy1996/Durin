#pragma once

#include "PackageV3Measurement.h"
#include "PackageV4ReferenceModel.h"

#include "DObject/Archive.h"

#include <array>

namespace Durin::Testing::DastV4
{
	struct FFeasibilityReport
	{
		std::array<uint64, SectionCount> SectionBytes{};
		uint64 EnvelopeAndDirectoryBytes = 0;
		uint64 TotalBytes = 0;
		uint64 NameCount = 0;
		uint64 TypeCount = 0;
		uint64 SchemaCount = 0;
		uint64 ObjectCount = 0;
		uint64 OverrideCount = 0;
		uint64 OmittedDefaultCount = 0;
		uint64 RetainedDescriptorBytes = 0;
		uint64 ParseOperations = 0;
		uint64 AllocationInputs = 0;
		uint32 MaximumNesting = 0;
		uint64 Digest = 0;

		auto operator==(const FFeasibilityReport&) const -> bool = default;
	};

	struct FFeasibilityPackage
	{
		std::vector<uint8> Bytes;
		FFeasibilityReport Report;
	};

	// Test-only bridge from the unified Archive contract into the frozen v4 type
	// vocabulary. It deliberately creates no production v4 save entry point.
	auto AdaptArchiveLogicalType(
		const FArchiveLogicalTypeDescriptor& Input,
		FTypePtr& OutType,
		std::string& OutError) -> bool;
	auto AddArchiveDiscoveredField(
		const FArchiveFieldDescriptor& Field,
		FTableInput& InOutTables,
		std::string& OutError) -> bool;

	// Converts a complete authored DAST v3 package into the generic v4 reference
	// model. ReverseDiscovery perturbs all order-insensitive discovery inputs.
	auto BuildFeasibilityPackageFromV3(
		std::span<const uint8> V3Bytes,
		bool ReverseDiscovery,
		FFeasibilityPackage& OutPackage,
		std::string& OutError) -> bool;
}
