#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBuildFunction.h"
#include "Modules/ModularFeature.h"

namespace Durin::DerivedData
{
	class IBuildFunctionFactory : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "DerivedDataBuildFunctionFactory";
		virtual auto GetFunction() const noexcept -> std::shared_ptr<const IBuildFunction> = 0;
	};

	// Registers for the lifetime of the factory. Sessions own the returned function.
	template<auto MakeFunction>
	class TBuildFunctionFactory final : public IBuildFunctionFactory
	{
	public:
		TBuildFunctionFactory()
		{
			IModularFeatures::Get().RegisterModularFeature(Type, this);
		}

		~TBuildFunctionFactory() override
		{
			IModularFeatures::Get().UnregisterModularFeature(Type, this);
		}

		TBuildFunctionFactory(const TBuildFunctionFactory&) = delete;
		auto operator=(const TBuildFunctionFactory&) -> TBuildFunctionFactory& = delete;

		auto GetFunction() const noexcept -> std::shared_ptr<const IBuildFunction> override { return Function; }

	private:
		const std::shared_ptr<const IBuildFunction> Function = MakeFunction();
		const FName Type{FeatureName};
	};
}
