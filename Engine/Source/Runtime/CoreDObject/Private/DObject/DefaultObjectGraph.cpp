#include "DObject/DefaultObjectGraph.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/Object.h"
#include "DObject/Property.h"

namespace Durin
{
	namespace
	{
		struct FDefaultObjectIdentity
		{
			std::string ClassName;
			std::string ObjectName;

			auto operator<=>(const FDefaultObjectIdentity&) const = default;
		};

		auto GetIdentity(const DObject* Object) -> FDefaultObjectIdentity
		{
			return {
				Object && Object->GetClass() ? Object->GetClass()->GetQualifiedName().ToString() : std::string{},
				Object ? Object->GetName() : std::string{}};
		}

		auto AppendPath(
			std::string_view Parent,
			const FDefaultObjectIdentity& Identity,
			std::string& OutPath
		) -> bool
		{
			const std::string Segment = std::format("/{}:{}", Identity.ClassName, Identity.ObjectName);
			if (Parent.size() + Segment.size() > PropertyIdentityMaxPathLength) return false;
			OutPath.assign(Parent);
			OutPath.append(Segment);
			return true;
		}
	}

	auto FDefaultObjectGraphMap::Build(
		const DObject* TemplateRoot,
		const DObject* InstanceRoot,
		FDefaultObjectGraphDiagnostic* OutDiagnostic
	) -> bool
	{
		Reset();
		if (OutDiagnostic) OutDiagnostic->Reset();
		auto Fail = [&](EDefaultObjectGraphFailureReason Reason, std::string_view Path) {
			Reset();
			if (OutDiagnostic)
			{
				OutDiagnostic->Reason = Reason;
				OutDiagnostic->LogicalPath.assign(Path);
			}
			return false;
		};
		if (!TemplateRoot || !InstanceRoot) return Fail(EDefaultObjectGraphFailureReason::InvalidInput, "<root>");
		if (!TemplateRoot->IsClassDefaultObject())
			return Fail(EDefaultObjectGraphFailureReason::InvalidTemplateRoot, TemplateRoot->GetName());
		if (InstanceRoot->IsTemplateObject())
			return Fail(EDefaultObjectGraphFailureReason::InvalidInstanceRoot, InstanceRoot->GetName());
		if (!TemplateRoot->GetClass() || TemplateRoot->GetClass() != InstanceRoot->GetClass())
			return Fail(EDefaultObjectGraphFailureReason::ClassMismatch, TemplateRoot->GetName());

		struct FPendingPair
		{
			const DObject* Template = nullptr;
			const DObject* Instance = nullptr;
			std::string Path;
		};
		std::vector<FPendingPair> Pending{{TemplateRoot, InstanceRoot, TemplateRoot->GetName()}};
		std::unordered_set<const DObject*> VisitedTemplates;
		std::unordered_set<const DObject*> VisitedInstances;
		while (!Pending.empty())
		{
			FPendingPair Pair = std::move(Pending.back());
			Pending.pop_back();
			if (!VisitedTemplates.insert(Pair.Template).second)
				return Fail(EDefaultObjectGraphFailureReason::TemplateCycle, Pair.Path);
			if (!VisitedInstances.insert(Pair.Instance).second)
				return Fail(EDefaultObjectGraphFailureReason::InstanceCycle, Pair.Path);
			if (TemplateToInstance.size() >= DefaultObjectGraphMaxObjects)
				return Fail(EDefaultObjectGraphFailureReason::ObjectLimit, Pair.Path);
			TemplateToInstance.emplace(Pair.Template, Pair.Instance);
			InstanceToTemplate.emplace(Pair.Instance, Pair.Template);

			std::vector<const DObject*> TemplateChildren;
			for (DObject* Child : GDObjectArray.GetObjectsWithOuter(
					 Pair.Template, EObjectQueryScope::IncludeTemplates, false))
			{
				if (!Child || Child->GetOuter() != Pair.Template)
					return Fail(EDefaultObjectGraphFailureReason::InvalidOuter, Pair.Path);
				if (!Child->HasAnyObjectFlags(EObjectFlags::DefaultSubobject))
					return Fail(EDefaultObjectGraphFailureReason::InvalidTemplateNode, Pair.Path);
				TemplateChildren.push_back(Child);
			}
			std::ranges::sort(TemplateChildren, [](const DObject* Left, const DObject* Right) {
				return GetIdentity(Left) < GetIdentity(Right);
			});
			for (size_t Index = 1; Index < TemplateChildren.size(); ++Index)
			{
				if (GetIdentity(TemplateChildren[Index - 1]) == GetIdentity(TemplateChildren[Index]))
					return Fail(EDefaultObjectGraphFailureReason::DuplicateTemplateIdentity, Pair.Path);
			}

			const std::vector<DObject*> InstanceChildren = GDObjectArray.GetObjectsWithOuter(
				Pair.Instance, EObjectQueryScope::LiveOnly, false);
			std::vector<FPendingPair> Children;
			Children.reserve(TemplateChildren.size());
			for (const DObject* TemplateChild : TemplateChildren)
			{
				const FDefaultObjectIdentity Identity = GetIdentity(TemplateChild);
				std::vector<const DObject*> Matches;
				bool bFoundNameWithDifferentClass = false;
				for (const DObject* InstanceChild : InstanceChildren)
				{
					if (!InstanceChild || InstanceChild->GetOuter() != Pair.Instance)
						return Fail(EDefaultObjectGraphFailureReason::InvalidOuter, Pair.Path);
					const FDefaultObjectIdentity InstanceIdentity = GetIdentity(InstanceChild);
					if (InstanceIdentity.ObjectName != Identity.ObjectName) continue;
					if (InstanceIdentity.ClassName == Identity.ClassName) Matches.push_back(InstanceChild);
					else bFoundNameWithDifferentClass = true;
				}
				std::string ChildPath;
				if (!AppendPath(Pair.Path, Identity, ChildPath))
					return Fail(EDefaultObjectGraphFailureReason::PathLimit, Pair.Path);
				if (Matches.empty())
					return Fail(
						bFoundNameWithDifferentClass
							? EDefaultObjectGraphFailureReason::ClassMismatch
							: EDefaultObjectGraphFailureReason::MissingInstanceNode,
						ChildPath);
				if (Matches.size() != 1)
					return Fail(EDefaultObjectGraphFailureReason::DuplicateInstanceIdentity, ChildPath);
				const DObject* InstanceChild = Matches.front();
				if (InstanceChild->IsTemplateObject())
					return Fail(EDefaultObjectGraphFailureReason::InvalidInstanceNode, ChildPath);
				if (TemplateChild->GetClass() != InstanceChild->GetClass())
					return Fail(EDefaultObjectGraphFailureReason::ClassMismatch, ChildPath);
				Children.push_back({TemplateChild, InstanceChild, std::move(ChildPath)});
			}
			for (auto It = Children.rbegin(); It != Children.rend(); ++It) Pending.push_back(std::move(*It));
		}
		return true;
	}

	auto FDefaultObjectGraphMap::FindInstance(const DObject* TemplateObject) const -> const DObject*
	{
		const auto It = TemplateToInstance.find(TemplateObject);
		return It == TemplateToInstance.end() ? nullptr : It->second;
	}

	auto FDefaultObjectGraphMap::FindTemplate(const DObject* InstanceObject) const -> const DObject*
	{
		const auto It = InstanceToTemplate.find(InstanceObject);
		return It == InstanceToTemplate.end() ? nullptr : It->second;
	}

	auto FDefaultObjectGraphMap::AreReferencesEquivalent(const DObject* Left, const DObject* Right) const -> bool
	{
		return Left == Right || FindInstance(Left) == Right || FindInstance(Right) == Left
			|| FindTemplate(Left) == Right || FindTemplate(Right) == Left;
	}
}
