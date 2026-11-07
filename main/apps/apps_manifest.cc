// Single place where built-in apps are registered. Adding an app = one line
// here plus its implementation; no dynamic loading and no runtime registry.
#include "app.h"

extern const AppInfo kAppCenterInfo;
extern const AppInfo kFlashlightInfo;
extern const AppInfo kRadioInfo;

App* CreateAppCenterApp();
App* CreateFlashlightApp();
App* CreateRadioApp();

static const AppEntry kEntries[] = {
    {&kAppCenterInfo, CreateAppCenterApp},
    {&kRadioInfo, CreateRadioApp},
    {&kFlashlightInfo, CreateFlashlightApp},
};

const AppEntry* GetAppRegistry(int* count) {
    if (count != nullptr) {
        *count = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));
    }
    return kEntries;
}
