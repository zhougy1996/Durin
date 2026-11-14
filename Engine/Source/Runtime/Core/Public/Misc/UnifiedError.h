#pragma once

#include "Misc/CoreStd.h"
#include "CoreAPI.h"

namespace Durin::UnifiedError
{
	// Declaration identifiers may be literals; FError owns a copy of their names.
	struct FErrorId
	{
		std::string_view Domain;
		std::string_view Code;
		constexpr auto operator==(const FErrorId&) const -> bool = default;
	};

	struct FErrorContext
	{
		std::string Key;
		std::string Value;
	};

	// A small owning error value. Absence/success belongs to optional or the operation.
	// Copies share immutable data; appending context detaches only the modified copy.
	// Shared data is allocated and destroyed in Core, including plugin errors.
	class FError
	{
	public:
		CORE_API explicit FError(FErrorId Id, std::string_view Message = {});
		CORE_API auto GetDomain() const -> const std::string&;
		CORE_API auto GetCode() const -> const std::string&;
		CORE_API auto Is(FErrorId Id) const -> bool;
		CORE_API auto GetMessage() const -> const std::string&;
		CORE_API auto GetContexts() const -> std::span<const FErrorContext>;
		CORE_API auto PushContext(std::string Key, std::string Value) -> FError&;
		CORE_API auto ToString() const -> std::string;
		// Error identity is independent of presentation and propagation context.
		CORE_API auto operator==(const FError& Other) const -> bool;
	private:
		struct FData;
		std::shared_ptr<FData> Data;
	};

	inline auto operator<<(std::ostream& Stream, const FError& Error) -> std::ostream&
	{
		return Stream << Error.ToString();
	}
}

template<>
struct std::formatter<Durin::UnifiedError::FError> : std::formatter<std::string_view>
{
	auto format(const Durin::UnifiedError::FError& Error, std::format_context& Context) const
	{
		return std::formatter<std::string_view>::format(Error.ToString(), Context);
	}
};
