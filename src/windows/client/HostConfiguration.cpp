#include "HostConfiguration.h"
#include "BrokerClient.h"

namespace stone::windows {
bool WindowsAppsUseDarkMode()
{
    return ReadClientHostState().ui.dark;
}

HostUiConfiguration ReadHostUiConfiguration()
{
    return ReadClientHostState().ui;
}
}
