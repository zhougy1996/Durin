#include "Misc/UnifiedError.h"
#include "Misc/Assertion.h"

namespace Durin::UnifiedError
{
	struct FError::FData
	{
		std::string Domain;
		std::string Code;
		std::string Message;
		std::vector<FErrorContext> Contexts;
	};

	FError::FError(FErrorId Id, std::string_view Message)
	{
		require(!Id.Domain.empty() && !Id.Code.empty());
		Data = std::make_shared<FData>(std::string(Id.Domain), std::string(Id.Code), std::string(Message));
	}
	auto FError::GetDomain() const -> const std::string& { require(Data); return Data->Domain; }
	auto FError::GetCode() const -> const std::string& { require(Data); return Data->Code; }
	auto FError::GetMessage() const -> const std::string& { require(Data); return Data->Message; }
	auto FError::GetContexts() const -> std::span<const FErrorContext> { require(Data); return Data->Contexts; }
	auto FError::Is(FErrorId Id) const -> bool
	{
		return Data && Data->Domain == Id.Domain && Data->Code == Id.Code;
	}
	auto FError::PushContext(std::string Key, std::string Value) -> FError&
	{
		require(Data);
		if (Data.use_count() != 1) Data = std::make_shared<FData>(*Data);
		Data->Contexts.push_back({std::move(Key), std::move(Value)});
		return *this;
	}
	auto FError::ToString() const -> std::string
	{
		require(Data);
		auto Text = std::format("{}::{}", Data->Domain, Data->Code);
		if (!Data->Message.empty()) Text += ": " + Data->Message;
		for (const auto& Context : Data->Contexts)
			Text += std::format("; {}: {}", Context.Key, Context.Value);
		return Text;
	}
	auto FError::operator==(const FError& Other) const -> bool
	{
		return Data == Other.Data || (Data && Other.Data
			&& Data->Domain == Other.Data->Domain && Data->Code == Other.Data->Code);
	}
}
