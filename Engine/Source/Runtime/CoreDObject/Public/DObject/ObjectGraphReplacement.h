#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/ObjectHandle.h"
#include "DObject/PropertyDiagnostic.h"
#include "DObject/ContainerOps.h"
#include <functional>

namespace Durin
{
	class DObject;
	class DPackage;
	class FObjectGraphReplacement;

	// No result reports partial publication. Stale and all preparation failures preserve live state.
	enum class EObjectReplacementError : uint8
	{
		None, InvalidGraph, IncompatibleType, UnmappedReference, Unsupported,
		MapCollision, BudgetExceeded, Stale, Busy, ParticipantRejected, AllocationFailure
	};

	enum class EObjectReplacementMapReason : uint8
	{
		None, PackageBudget, InvalidPackagePair, InvalidObject, DuplicateIdentity,
		ObjectBudget, IncompatibleType,
	};

	struct FObjectReplacementMapError
	{
		EObjectReplacementError Code = EObjectReplacementError::None;
		EObjectReplacementMapReason Reason = EObjectReplacementMapReason::None;
		size_t PackageIndex = 0;
		uint64 ActualCount = 0;
		uint64 MaximumCount = 0;
		std::string ObjectPath;
		std::string ExpectedType;
		std::string ActualType;
		auto HasError() const -> bool { return Code != EObjectReplacementError::None; }
	};

	struct FObjectReplacementMapResult
	{
		FObjectReplacementMapError Error;
		auto Succeeded() const -> bool { return !Error.HasError(); }
		explicit operator bool() const { return Succeeded(); }
	};
	COREDOBJECT_API auto FormatObjectReplacementMapError(const FObjectReplacementMapError& Error) -> std::string;

	enum class EObjectReplacementReason : uint8
	{
		None,
		DetachedUnmappedReference,
		ArrayMutableTraversal,
		ArrayTraversal,
		MapOperations,
		MapTransactionalOperations,
		MapAllocation,
		MapValueCopy,
		MapInsertion,
		MapTraversal,
		ReferenceMetadata,
		ReferenceBudget,
		ReferenceWriter,
		TemplateReference,
		ExternalUnmappedReference,
		ReferenceType,
		StructMetadata,
		NativeStructAdapter,
		ArrayReferenceMetadata,
		MapReferenceMetadata,
		ReferenceTraversal,
		ContainerCommit,
		ContainerCopy,
		ContainerComparison,
		NativeOwnerParticipant,
		AlreadyActive,
		PackageReservation,
		InvalidParticipant,
		RootedObject,
		StrongOwnerClaim,
		PrepareAllocation,
		PrepareException,
		NotPrepared,
		ObjectMembershipChanged,
		ObjectIdentityChanged,
		ObjectRootChanged,
		PackageChanged,
		ParticipantChanged,
		StrongOwnerChanged,
		ReferenceSlotsChanged,
		ContainerChanged,
		ValidationMutation,
		ValidateAllocation,
		ValidateException,
		ReplacementMap,
		ParticipantBusy,
		ParticipantUnmappedPackage,
		ParticipantRejected,
		PersistenceRejected,
	};
	struct FObjectReplacementError
	{
		EObjectReplacementError Code = EObjectReplacementError::None;
		EObjectReplacementReason Reason = EObjectReplacementReason::None;
		std::string ObjectPath;
		std::string PropertyName;
		std::string ExpectedType;
		std::string ActualType;
		std::vector<std::string> Route;
		uint32 ArrayIndex = 0;
		uint64 ActualCount = 0;
		uint64 MaximumCount = 0;
		uint64 ExpectedCount = 0;
		uint64 ActualRevision = 0;
		uint64 ExpectedRevision = 0;
		size_t ParticipantIndex = 0;
		std::variant<std::monostate, FObjectReplacementMapError, FPropertyValueError, EContainerOpResult> Cause;
	};
	struct FObjectReplacementResult
	{
		FObjectReplacementError Error;
		auto Succeeded() const -> bool { return Error.Code == EObjectReplacementError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	COREDOBJECT_API auto FormatObjectReplacementError(const FObjectReplacementError& Error) -> std::string;

	// Current stays registered until Prepared and every reference plan can commit together.
	// A null Current denotes a new package; its reserved path remains invisible until commit.
	struct FObjectReplacementPackagePair
	{
		DPackage* Current = nullptr;
		DPackage* Prepared = nullptr;
	};

	struct FObjectReplacementBudget
	{
		uint64 MaximumPackages = 16;
		uint64 MaximumObjects = 65536;
		uint64 MaximumReferenceSlots = 1048576;
	};

	// Maps package-relative Outer/name identity; a null replacement denotes a removed object.
	class FObjectReplacementMap
	{
	public:
		struct FEntry
		{
			DObject* Previous = nullptr;
			DObject* Replacement = nullptr;
		};
		COREDOBJECT_API auto Build(std::span<const FObjectReplacementPackagePair> Packages,
			const FObjectReplacementBudget& Budget = {}) -> FObjectReplacementMapResult;
		COREDOBJECT_API auto Find(DObject* Previous) const -> const FEntry*;
		auto GetEntries() const -> std::span<const FEntry> { return Entries; }
		auto GetPreparedObjects() const -> std::span<DObject* const> { return PreparedObjects; }

	private:
		std::vector<FEntry> Entries;
		std::unordered_map<DObject*, size_t> Indices;
		std::vector<DObject*> PreparedObjects;
	};

	// Native owners must retain their storage/module for the operation's lifetime.
	// Prepare may fail. Validate is observational. Commit/Abort cannot allocate,
	// broadcast, reenter replacement/GC, throw, or invoke fallible resource work.
	class IObjectReplacementParticipant
	{
	public:
		virtual ~IObjectReplacementParticipant() = default;
		virtual auto Prepare(const FObjectReplacementMap& Map) -> FObjectReplacementResult = 0;
		virtual auto Validate() const -> bool = 0;
		virtual auto CoversNativeReferences(const DObject& Owner) const -> bool = 0;
		// Counts exact old-generation FStrongObjectPtr owners rebound by this participant.
		virtual auto GetStrongReferenceCount(const DObject& Object) const -> uint32 = 0;
		virtual auto Commit() noexcept -> void = 0;
		virtual auto Abort() noexcept -> void = 0;
		virtual auto CanRetire() const -> bool = 0;
	};

	// Owns detached container storage; GC enumeration is never a write API.
	class FObjectReferenceReplacementPlan
	{
	public:
		COREDOBJECT_API FObjectReferenceReplacementPlan();
		COREDOBJECT_API ~FObjectReferenceReplacementPlan();
		FObjectReferenceReplacementPlan(const FObjectReferenceReplacementPlan&) = delete;
		auto operator=(const FObjectReferenceReplacementPlan&) -> FObjectReferenceReplacementPlan& = delete;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		friend class FObjectGraphReplacement;
	};

	// GameThread-only, single-operation memory publication. Prepare takes ownership of
	// candidate graphs only on success. Abort retires accepted candidates through GC.
	// After TryCommit succeeds, retain this owner until Retire succeeds; destruction
	// before runtime participants acknowledge retirement is a contract violation.
	class FObjectGraphReplacement
	{
	public:
		COREDOBJECT_API FObjectGraphReplacement();
		COREDOBJECT_API ~FObjectGraphReplacement();
		FObjectGraphReplacement(const FObjectGraphReplacement&) = delete;
		auto operator=(const FObjectGraphReplacement&) -> FObjectGraphReplacement& = delete;
		COREDOBJECT_API auto Prepare(std::span<const FObjectReplacementPackagePair> Packages,
			std::span<const std::shared_ptr<IObjectReplacementParticipant>> Participants = {},
			const FObjectReplacementBudget& Budget = {}) -> FObjectReplacementResult;
		// Revalidates and consumes the prepared writes without yielding to an observer.
		// Optional persistence runs after final validation and before the non-failing
		// memory commit. It must be synchronous, must not mutate object graphs or
		// reenter replacement/GC, and must roll back its own writes on failure.
		COREDOBJECT_API auto TryCommit(
			const std::function<FObjectReplacementResult()>& Persist = {}) -> FObjectReplacementResult;
		COREDOBJECT_API auto Abort() noexcept -> void;
		// False means a participant or new external strong reference still retains the old graph.
		COREDOBJECT_API auto Retire() -> bool;
		COREDOBJECT_API auto GetMap() const -> const FObjectReplacementMap&;
		COREDOBJECT_API auto OwnsPreparedPackage(const DPackage& Package) const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		auto CommitPrepared() noexcept -> void;
	};
}
