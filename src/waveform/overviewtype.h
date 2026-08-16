#pragma once

// required for Qt-Macros
#include <qobjectdefs.h>

namespace mixxx {
Q_NAMESPACE

enum class OverviewType {
    Filtered,
    HSV,
    RGB,
    // New types are appended: the value is stored in the settings as a number
    // and existing ones must keep meaning what they meant.
    Spectrum,
};
Q_ENUM_NS(OverviewType);

} // namespace mixxx
