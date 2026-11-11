#pragma once

#include "ps2x/iop/iop_service.h"

#include <memory>
#include <vector>

namespace ps2x::iop::detail
{
    using ps2x::iop::IopService;
    using ServiceList = std::vector<std::unique_ptr<IopService>>;
}
