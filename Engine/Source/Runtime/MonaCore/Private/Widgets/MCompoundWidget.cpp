#include "Widgets/MCompoundWidget.h"

namespace Doge::Mona
{
	void MCompoundWidget::OnRender()
	{
		ChildWidget->Render();
	}
} // namespace Doge::Mona