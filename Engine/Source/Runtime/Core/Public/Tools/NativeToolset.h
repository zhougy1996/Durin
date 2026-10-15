#pragma once

#include "CoreMinimal.h"
#include "Tools/Tool.h"

namespace Durin
{
	// Owned definition for explicitly bound native functions or dynamic adapters.
	struct FToolBinding
	{
		FToolDescriptor Descriptor;
		std::function<FToolResult(const FToolRequest&, const FToolContext&)> Handler;
		std::function<FToolAvailability(const FToolRequest&, const FToolContext&)> Availability;
	};

	// Immutable after construction. The module retains this object and its feature token.
	class FNativeToolset final : public IToolset
	{
	public:
		CORE_API FNativeToolset(std::string Name, std::string Description, std::vector<FToolBinding> Tools);
		CORE_API auto DescribeToolset() const -> FToolsetDescriptor override;
		CORE_API auto CheckAvailability(const FToolRequest&, const FToolContext&) -> FToolAvailability override;
		// Requires arguments prepared against this toolset descriptor by the calling host.
		CORE_API auto Call(const FToolRequest&, const FToolContext&) -> FToolResult override;
	private:
		auto Find(const FToolRequest&) const -> const FToolBinding*;
		FToolsetDescriptor Descriptor;
		std::vector<FToolBinding> Bindings;
	};

	// Specialize for additional owned value types; no DObject or function reflection is required.
	template<class T> struct TToolValueCodec
	{
		static_assert(std::same_as<T, bool> || std::integral<T> || std::floating_point<T> || std::same_as<T, std::string>,
			"Tool value type requires an explicit TToolValueCodec specialization.");
		static auto Schema() -> FToolSchema
		{
			if constexpr (std::same_as<T, bool>) return FToolSchema(EToolValueType::Boolean);
			else if constexpr (std::integral<T>)
			{
				static_assert(sizeof(T) <= sizeof(uint64));
				FToolSchema Result(std::signed_integral<T> ? EToolValueType::Integer : EToolValueType::UnsignedInteger);
				Result.Minimum = std::numeric_limits<T>::lowest();
				Result.Maximum = std::numeric_limits<T>::max();
				return Result;
			}
			else if constexpr (std::floating_point<T>)
			{
				static_assert(sizeof(T) <= sizeof(double));
				FToolSchema Result(EToolValueType::Number);
				Result.Minimum = static_cast<double>(std::numeric_limits<T>::lowest());
				Result.Maximum = static_cast<double>(std::numeric_limits<T>::max());
				return Result;
			}
			else return FToolSchema(EToolValueType::String);
		}
		static auto Decode(const FToolValue& Value) -> T
		{
			if constexpr (std::same_as<T, bool> || std::same_as<T, std::string>) return std::get<T>(Value.Data);
			else if constexpr (std::signed_integral<T>) return static_cast<T>(std::get<int64>(Value.Data));
			else if constexpr (std::unsigned_integral<T>) return static_cast<T>(std::get<uint64>(Value.Data));
			else return static_cast<T>(std::get<double>(Value.Data));
		}
		static auto Encode(const T& Value) -> FToolValue
		{
			if constexpr (std::floating_point<T>) return static_cast<double>(Value);
			else return Value;
		}
	};

	template<class T> struct TToolValueCodec<std::vector<T>>
	{
		static auto Schema() -> FToolSchema
		{
			FToolSchema Result(EToolValueType::Array);
			Result.Items = std::make_unique<FToolSchema>(TToolValueCodec<T>::Schema());
			return Result;
		}
		static auto Decode(const FToolValue& Value) -> std::vector<T>
		{
			const auto& Items = std::get<FToolValue::FArray>(Value.Data);
			std::vector<T> Result;
			Result.reserve(Items.size());
			for (const auto& Item : Items) Result.push_back(TToolValueCodec<T>::Decode(Item));
			return Result;
		}
		static auto Encode(const std::vector<T>& Value) -> FToolValue
		{
			FToolValue::FArray Result;
			Result.reserve(Value.size());
			for (const auto& Item : Value) Result.push_back(TToolValueCodec<T>::Encode(Item));
			return Result;
		}
	};

	struct FToolParameter
	{
		std::string Name;
		std::string Description;
		std::optional<FToolValue> Default;
	};

	// Parameter names and policy are explicit; C++ types supply input/output schemas and conversion.
	// Only value/const-reference arguments are accepted; output references and engine pointers need adapters.
	template<class R, class... Args>
	auto BindTool(FToolDescriptor Descriptor, R (*Function)(const FToolContext&, Args...),
		std::array<FToolParameter, sizeof...(Args)> Parameters) -> FToolBinding
	{
		static_assert(((!std::is_reference_v<Args> || (std::is_lvalue_reference_v<Args>
			&& std::is_const_v<std::remove_reference_t<Args>>)) && ...));
		static_assert(!std::is_reference_v<R> && !std::is_pointer_v<R>);
		if (!Function) throw std::invalid_argument("A tool binding requires a function.");
		Descriptor.Input = FToolSchema(EToolValueType::Object);
		auto Add = [&]<class T>(const FToolParameter& Parameter) {
			if (Parameter.Name.empty() || Descriptor.Input.Properties.contains(Parameter.Name))
				throw std::invalid_argument("Tool parameter names must be nonempty and unique.");
			auto Schema = TToolValueCodec<std::remove_cvref_t<T>>::Schema();
			Schema.Description = Parameter.Description;
			Schema.Default = Parameter.Default;
			Descriptor.Input.Properties.emplace(Parameter.Name, std::move(Schema));
			Descriptor.Input.Required.push_back(Parameter.Name);
		};
		[&]<size_t... I>(std::index_sequence<I...>) { (Add.template operator()<Args>(Parameters[I]), ...); }(std::index_sequence_for<Args...>{});
		if constexpr (std::is_void_v<R>) Descriptor.Output = FToolSchema(EToolValueType::Null);
		else Descriptor.Output = TToolValueCodec<R>::Schema();
		if (auto Check = ValidateToolDescriptor(Descriptor); !Check) throw std::invalid_argument(Check.error().Message);
		FToolBinding Binding;
		Binding.Descriptor = std::move(Descriptor);
		Binding.Handler = [Function, Parameters = std::move(Parameters)](const FToolRequest& Request, const FToolContext& Context) -> FToolResult {
			const auto& Object = std::get<FToolValue::FObject>(Request.Arguments.Data);
			return [&]<size_t... I>(std::index_sequence<I...>) -> FToolResult {
				if constexpr (std::is_void_v<R>)
				{
					Function(Context, TToolValueCodec<std::remove_cvref_t<Args>>::Decode(Object.at(Parameters[I].Name))...);
					return {EToolStatus::Succeeded};
				}
				else return {EToolStatus::Succeeded, TToolValueCodec<R>::Encode(
					Function(Context, TToolValueCodec<std::remove_cvref_t<Args>>::Decode(Object.at(Parameters[I].Name))...))};
			}(std::index_sequence_for<Args...>{});
		};
		return Binding;
	}
}
