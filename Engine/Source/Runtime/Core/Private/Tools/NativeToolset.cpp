#include "Tools/NativeToolset.h"

namespace Durin
{
	FNativeToolset::FNativeToolset(std::string Name, std::string Description, std::vector<FToolBinding> Tools)
		: Descriptor{std::move(Name), std::move(Description), {}}, Bindings(std::move(Tools))
	{
		if (auto Check = ValidateToolsetDescriptor(Descriptor); !Check) throw std::invalid_argument(Check.error().Message);
		std::set<std::string> Names;
		for (const auto& Binding : Bindings)
		{
			if (static_cast<bool>(Binding.Handler) == static_cast<bool>(Binding.AsyncHandler) || !Names.insert(Binding.Descriptor.Name).second) throw std::invalid_argument("Tool bindings require unique names and handlers.");
			if (auto Check = ValidateToolDescriptor(Binding.Descriptor); !Check) throw std::invalid_argument(Check.error().Message);
			Descriptor.Tools.push_back(Binding.Descriptor);
		}
	}
	auto FNativeToolset::DescribeToolset() const -> FToolsetDescriptor { return Descriptor; }
	auto FNativeToolset::Find(const FToolRequest& Request) const -> const FToolBinding*
	{
		if (Request.Address.Toolset != Descriptor.Name) return nullptr;
		for (const auto& Binding : Bindings) if (Binding.Descriptor.Name == Request.Address.Tool) return &Binding;
		return nullptr;
	}
	auto FNativeToolset::CheckAvailability(const FToolRequest& Request, const FToolContext& Context) -> FToolAvailability
	{
		const auto* Binding = Find(Request);
		if (!Binding) return {false, {{EToolError::ToolUnavailable, "Tool is not owned by this toolset.", {}}}};
		if (Request.ContractVersion != Binding->Descriptor.ContractVersion) return {false, {{EToolError::VersionMismatch, "Tool contract version does not match.", {}}}};
		return Binding->Availability ? Binding->Availability(Request, Context) : FToolAvailability{};
	}
	auto FNativeToolset::Call(const FToolRequest& Request, const FToolContext& Context) -> FToolFuture
	{
		const auto* Binding = Find(Request);
		if (!Binding) return ReadyToolResult({EToolStatus::Failed, {}, {{EToolError::ToolUnavailable, "Tool is not owned by this toolset.", {}}}});
		if (Request.ContractVersion != Binding->Descriptor.ContractVersion)
			return ReadyToolResult({EToolStatus::Failed, {}, {{EToolError::VersionMismatch, "Tool contract version does not match.", {}}}});
		return Binding->AsyncHandler ? Binding->AsyncHandler(Request, Context) : ReadyToolResult(Binding->Handler(Request, Context));
	}
}
