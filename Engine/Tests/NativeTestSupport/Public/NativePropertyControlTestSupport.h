#pragma once

#include "PropertyEditor/PropertyControl.h"

namespace Durin::Testing
{
	// Simulate one interactive widget input, including synchronous notifications.
	template<typename TValue>
	auto ApplyControlInput(PropertyEditor::FPropertyControl& Control, const TValue& Value, std::string* Error = nullptr) -> bool
	{
		using namespace PropertyEditor;
		std::string Diagnostic;
		bool bWritten = false;
		{
			FPropertyControl::FInput Input(Control, &Diagnostic);
			if (Input)
			{
				if constexpr (std::is_same_v<TValue, FPropertyValueSnapshot>)
					bWritten = Input->SetValueFromSnapshot(Value, FPropertyControl::InteractiveFlags).has_value();
				else
					bWritten = Input->SetValue(Value, FPropertyControl::InteractiveFlags) == FPropertyAccess::Success;
				if (!bWritten) Diagnostic = Input->GetLastError();
			}
		}
		if (Error && !Diagnostic.empty()) *Error = Diagnostic;
		return bWritten && Diagnostic.empty();
	}
}
