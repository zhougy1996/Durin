#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/Archive.h"
#include "DObject/AuthoredOverrideLedger.h"
#include "DObject/DefaultObjectGraph.h"
#include "DObject/Property.h"

namespace Durin
{
	enum class EDefaultDeltaMode : uint8 { Enabled, NoDelta };
	enum class EDefaultDeltaBaselineKind : uint8 { None, ClassDefault, StructTypeDefault };
	enum class EDefaultDeltaDisposition : uint8 { Omitted, Emitted };
	enum class EDefaultDeltaProvenance : uint8 { None, Explicit, Forced };

	enum class EDefaultDeltaFailureReason : uint8
	{
		None,
		InvalidInput,
		MissingClassDefault,
		DefaultObjectGraphFailure,
		ArchiveFailure,
		ManifestMismatch,
		DuplicateField,
		UnsupportedLogicalType,
		UnsupportedIdentity,
		MissingStructDefault,
		DepthLimit,
		FieldLimit,
		PathLimit,
		AuthoredOverrideFailure,
	};

	inline constexpr uint32 DefaultDeltaMaxDepth = 64;
	inline constexpr uint64 DefaultDeltaMaxFields = 1'000'000;
	inline constexpr size_t DefaultDeltaMaxPathLength = 1024;

	struct FDefaultDeltaDiagnostic
	{
		EDefaultDeltaFailureReason Reason = EDefaultDeltaFailureReason::None;
		std::string LogicalPath;
		EPropertyIdentityReason IdentityReason = EPropertyIdentityReason::None;
		EDefaultObjectGraphFailureReason GraphReason = EDefaultObjectGraphFailureReason::None;
		EArchiveFailureCode ArchiveReason = EArchiveFailureCode::InvalidData;
		EAuthoredOverrideFailureReason AuthoredOverrideReason = EAuthoredOverrideFailureReason::None;

		auto Reset() -> void { *this = {}; }
	};

	struct FDefaultDeltaNode;

	struct FDefaultDeltaFieldPlan
	{
		FArchiveFieldDescriptor Descriptor;
		EDefaultDeltaBaselineKind Baseline = EDefaultDeltaBaselineKind::None;
		EDefaultDeltaDisposition Disposition = EDefaultDeltaDisposition::Emitted;
		EDefaultDeltaProvenance Provenance = EDefaultDeltaProvenance::Explicit;
		EPropertyIdentityResult Identity = EPropertyIdentityResult::Different;
		std::shared_ptr<FDefaultDeltaNode> Value;
	};

	struct FDefaultDeltaNode
	{
		FArchiveLogicalTypeDescriptor LogicalType;
		EDefaultDeltaBaselineKind Baseline = EDefaultDeltaBaselineKind::None;
		EDefaultDeltaDisposition Disposition = EDefaultDeltaDisposition::Emitted;
		EDefaultDeltaProvenance Provenance = EDefaultDeltaProvenance::Explicit;
		EPropertyIdentityResult Identity = EPropertyIdentityResult::Different;
		bool BoolValue = false;
		int64 SignedValue = 0;
		uint64 UnsignedValue = 0;
		uint64 FloatingBits = 0;
		std::string TextValue;
		FGuid GuidValue;
		std::vector<uint8> ByteValue;
		std::vector<uint8> CanonicalMapKeyToken;
		DObject* ObjectValue = nullptr;
		bool bHasAtomicValue = false;
		// Capture-only source used for authoritative identity; cleared from published plans.
		const void* SourceValue = nullptr;
		DStruct* SourceStruct = nullptr;
		std::vector<FDefaultDeltaFieldPlan> Fields;
		std::vector<std::shared_ptr<FDefaultDeltaNode>> Elements;
	};

	struct FDefaultDeltaObjectPlan
	{
		const DObject* Object = nullptr;
		const DObject* ClassDefaultObject = nullptr;
		std::vector<FDefaultDeltaFieldPlan> Fields;
	};

	struct FDefaultDeltaPlan
	{
		EDefaultDeltaMode Mode = EDefaultDeltaMode::Enabled;
		std::vector<FDefaultDeltaObjectPlan> Objects;
		uint64 FieldCount = 0;
		uint64 EmittedFieldCount = 0;
		uint64 OmittedFieldCount = 0;
		uint64 ComparisonCount = 0;
		uint32 MaximumDepth = 0;

		auto Reset() -> void { *this = {}; }
	};

	COREDOBJECT_API auto AreArchiveLogicalTypesEquivalent(
		const FArchiveLogicalTypeDescriptor& Left,
		const FArchiveLogicalTypeDescriptor& Right) -> bool;

	COREDOBJECT_API auto BuildDefaultDeltaPlan(
		DObject* RootObject,
		EDefaultDeltaMode Mode,
		FDefaultDeltaPlan& OutPlan,
		FDefaultDeltaDiagnostic* OutDiagnostic = nullptr) -> bool;

	COREDOBJECT_API auto AreDefaultDeltaPlansEquivalent(
		const FDefaultDeltaPlan& Left,
		const FDefaultDeltaPlan& Right) -> bool;
}
