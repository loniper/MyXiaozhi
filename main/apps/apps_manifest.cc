// Single place where built-in apps are registered. Adding an app = one line
// here plus its implementation; no dynamic loading and no runtime registry.
#include "app.h"

extern const AppInfo kAppCenterInfo;
extern const AppInfo kDemoInfo;
extern const AppInfo kDeviceInfoInfo;
extern const AppInfo kRadioInfo;

App* CreateAppCenterApp();
App* CreateDemoApp();
App* CreateDeviceInfoApp();
App* CreateRadioApp();

static const AppEntry kEntries[] = {
    {&kAppCenterInfo, CreateAppCenterApp},
    {&kRadioInfo, CreateRadioApp},
    {&kDeviceInfoInfo, CreateDeviceInfoApp},
    {&kDemoInfo, CreateDemoApp},
};

const AppEntry* GetAppRegistry(int* count) {
    if (count != nullptr) {
        *count = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));
    }
    return kEntries;
}
