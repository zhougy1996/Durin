#include "Widgets/MCompoundWidget.h"

namespace Durin::Mona
{
	void MCompoundWidget::Draw()
	{
		ChildWidget->Draw();
	}
} // namespace Durin::Mona