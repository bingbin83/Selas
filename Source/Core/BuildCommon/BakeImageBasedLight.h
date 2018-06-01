#pragma once

//==============================================================================
// Joe Schutte
//==============================================================================

#include <SystemLib/Error.h>
#include <SystemLib/BasicTypes.h>

namespace Selas
{
    struct ImageBasedLightResourceData;

    //==============================================================================
    Error BakeImageBasedLight(const ImageBasedLightResourceData* data, cpointer filepath);
}
