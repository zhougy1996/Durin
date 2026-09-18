#pragma once
#include "CoreDObjectAPI.h"

namespace Durin
{
	// Higher modules own their typed payload and its presentation formatter.
	// The framework never formats a cause while deciding admission.
	class IObjectValidationCause
	{
	public:
		virtual ~IObjectValidationCause() = default;
		virtual auto Format() const -> std::string = 0;
	};
	enum class EObjectValidationError : uint8 { None, InvalidOwnedGraph, ModuleRejected, StructRejected, PropertyRejected };
	enum class EPropertyEditRejection : uint8
	{
		None, InvalidMetadata, IncompatibleObject, NonFiniteValue, DegenerateDirection,
		ParentCycle, ReentrantEdit, IncompleteDraft, Rejected, ModuleRejected,
	};
	struct FObjectValidationError
	{
		EObjectValidationError Code = EObjectValidationError::None;
		std::string ObjectPath;
		std::string StructName;
		uint32 SourceVersion = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		EPropertyEditRejection PropertyReason = EPropertyEditRejection::None;
		std::string PropertyName;
		std::shared_ptr<const IObjectValidationCause> Cause;
	};
	struct FObjectValidationResult
	{
		FObjectValidationError Error;
		auto Succeeded() const -> bool { return Error.Code == EObjectValidationError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	class DObject;
	struct FPropertyEditProposal;
	COREDOBJECT_API auto RejectPropertyEdit(const DObject& Object, const FPropertyEditProposal& Proposal,
		EPropertyEditRejection Reason, std::shared_ptr<const IObjectValidationCause> Cause = {}) -> FObjectValidationResult;
	COREDOBJECT_API auto FormatObjectValidationError(const FObjectValidationError& Error) -> std::string;
}
