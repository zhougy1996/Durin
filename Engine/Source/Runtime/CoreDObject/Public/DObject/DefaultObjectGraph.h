#pragma once

#include "CoreDObjectAPI.h"

namespace Durin
{
	class DObject;

	inline constexpr uint64 DefaultObjectGraphMaxObjects = 1'000'000;

	enum class EDefaultObjectGraphFailureReason : uint8
	{
		None,
		InvalidInput,
		InvalidTemplateRoot,
		InvalidInstanceRoot,
		InvalidTemplateNode,
		InvalidInstanceNode,
		ClassMismatch,
		InvalidOuter,
		DuplicateTemplateIdentity,
		MissingInstanceNode,
		DuplicateInstanceIdentity,
		TemplateCycle,
		InstanceCycle,
		ObjectLimit,
		PathLimit,
	};

	struct FDefaultObjectGraphDiagnostic
	{
		EDefaultObjectGraphFailureReason Reason = EDefaultObjectGraphFailureReason::None;
		std::string LogicalPath;

		auto Reset() -> void
		{
			Reason = EDefaultObjectGraphFailureReason::None;
			LogicalPath.clear();
		}
	};

	// Pairs one immutable class-default/default-subobject tree with one live tree.
	class FDefaultObjectGraphMap
	{
	public:
		COREDOBJECT_API auto Build(
			const DObject* TemplateRoot,
			const DObject* InstanceRoot,
			FDefaultObjectGraphDiagnostic* OutDiagnostic = nullptr) -> bool;

		COREDOBJECT_API auto FindInstance(const DObject* TemplateObject) const -> const DObject*;
		COREDOBJECT_API auto FindTemplate(const DObject* InstanceObject) const -> const DObject*;
		COREDOBJECT_API auto AreReferencesEquivalent(const DObject* Left, const DObject* Right) const -> bool;

		auto Num() const -> uint64 { return static_cast<uint64>(TemplateToInstance.size()); }
		auto IsEmpty() const -> bool { return TemplateToInstance.empty(); }
		auto Reset() -> void
		{
			TemplateToInstance.clear();
			InstanceToTemplate.clear();
		}

	private:
		std::unordered_map<const DObject*, const DObject*> TemplateToInstance;
		std::unordered_map<const DObject*, const DObject*> InstanceToTemplate;
	};
}
