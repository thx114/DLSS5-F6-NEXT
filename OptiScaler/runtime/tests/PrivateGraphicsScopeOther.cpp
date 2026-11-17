#include <runtime/PrivateGraphicsScope.h>

bool PrivateGraphicsScopeActiveFromOtherTranslationUnit()
{
    return PrivateGraphics::Active();
}
