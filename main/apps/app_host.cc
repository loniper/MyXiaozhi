#include "app_host.h"

#include <cstring>

void AppContext::RequestExit() {
    if (host_ != nullptr) {
        host_->RequestExitAsync();
    }
}

void AppContext::SwitchTo(const char* id) {
    if (host_ != nullptr) {
        host_->SwitchToAsync(id);
    }
}

bool AppHost::Init(lv_obj_t* parent, int width, int height, const lv_font_t* font, Hooks hooks) {
    if (parent == nullptr || layer_ != nullptr) {
        return false;
    }
    hooks_ = std::move(hooks);
    font_ = font;
    width_ = width;
    height_ = height;
    layer_ = lv_obj_create(parent);
    if (layer_ == nullptr) {
        return false;
    }
    lv_obj_remove_style_all(layer_);
    lv_obj_set_size(layer_, width, height);
    lv_obj_align(layer_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(layer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(layer_, LV_OPA_COVER, 0);
    lv_obj_add_flag(layer_, LV_OBJ_FLAG_HIDDEN);
    return true;
}

const AppEntry* AppHost::Find(const char* id) const {
    if (id == nullptr) {
        return nullptr;
    }
    int count = 0;
    const AppEntry* entries = GetAppRegistry(&count);
    for (int i = 0; i < count; ++i) {
        const AppInfo* info = entries[i].info;
        if (info == nullptr) {
            continue;
        }
        if (info->id != nullptr && strcmp(info->id, id) == 0) {
            return &entries[i];
        }
        // 语音指令里的应用名不一定等于内部 id（例如“网络电台”），按名称也认一次。
        if (info->name != nullptr && strcmp(info->name, id) == 0) {
            return &entries[i];
        }
    }
    return nullptr;
}

const AppInfo* AppHost::active_info() const {
    return entry_ != nullptr ? entry_->info : nullptr;
}

bool AppHost::Enter(const char* id) {
    if (layer_ == nullptr || app_ != nullptr || exit_pending_) {
        return false;
    }
    const AppEntry* entry = Find(id);
    if (entry == nullptr) {
        return false;
    }
    if (hooks_.on_enter) {
        hooks_.on_enter();
    }
    if (!EnterInternal(id)) {
        if (hooks_.on_exit) {
            hooks_.on_exit();
        }
        return false;
    }
    return true;
}

bool AppHost::EnterInternal(const char* id) {
    const AppEntry* entry = Find(id);
    if (entry == nullptr || layer_ == nullptr) {
        return false;
    }
    lv_obj_clean(layer_);
    lv_obj_remove_flag(layer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(layer_);
    app_ = entry->create();
    if (app_ == nullptr) {
        lv_obj_add_flag(layer_, LV_OBJ_FLAG_HIDDEN);
        return false;
    }
    entry_ = entry;
    ctx_ = new AppContext(this, layer_, width_, height_, font_);
    app_->OnEnter(*ctx_);
    return true;
}

void AppHost::SwitchToAsync(const char* id) {
    if (layer_ == nullptr || id == nullptr) {
        return;
    }
    if (app_ == nullptr) {
        Enter(id);
        return;
    }
    const AppInfo* active = active_info();
    if (active != nullptr && strcmp(active->id, id) == 0) {
        return;
    }
    pending_id_ = id;
    lv_async_call(DeferredSwitch, this);
}

void AppHost::DeferredSwitch(void* arg) {
    AppHost* host = static_cast<AppHost*>(arg);
    std::string id = host->pending_id_;
    host->pending_id_.clear();
    if (id.empty() || host->app_ == nullptr) {
        return;
    }
    // Tear the scene down, but keep the desktop suspended across the switch.
    host->suppress_exit_hook_ = true;
    host->DoExit();
    host->suppress_exit_hook_ = false;
    if (!host->EnterInternal(id.c_str())) {
        if (host->hooks_.on_exit) {
            host->hooks_.on_exit();
        }
    }
}

void AppHost::RequestExitAsync() {
    if (app_ == nullptr || exit_pending_) {
        return;
    }
    exit_pending_ = true;
    // Never delete the scene from inside its own event handler.
    lv_async_call(DeferredExit, this);
}

void AppHost::DeferredExit(void* arg) {
    static_cast<AppHost*>(arg)->DoExit();
}

void AppHost::Exit() {
    if (app_ == nullptr) {
        return;
    }
    DoExit();
}

void AppHost::DoExit() {
    exit_pending_ = false;
    if (app_ == nullptr) {
        return;
    }
    app_->OnExit();
    // 兜底保护：应用可能把事件回调直接挂在 layer_（应用层对象本身）上，而
    // lv_obj_clean() 只删子对象、摘不掉对象自身的回调。若应用在 OnExit 里漏了
    // lv_obj_remove_event_cb，下面 delete 之后这些回调的 user_data 就成了野指针，
    // 下一个应用一触摸就会跳进已析构的对象 —— 这里统一摘掉所有 user_data 指向
    // 本实例的回调，避免任何一个应用漏摘就把整机带崩。
    if (layer_ != nullptr) {
        lv_obj_remove_event_cb_with_user_data(layer_, nullptr, app_);
    }
    delete app_;
    app_ = nullptr;
    delete ctx_;
    ctx_ = nullptr;
    entry_ = nullptr;
    overlay_paused_ = false;
    if (layer_ != nullptr) {
        lv_obj_clean(layer_);
        lv_obj_add_flag(layer_, LV_OBJ_FLAG_HIDDEN);
    }
    if (hooks_.on_exit && !suppress_exit_hook_) {
        hooks_.on_exit();
    }
}

void AppHost::PauseForOverlay(bool paused) {
    if (app_ == nullptr || overlay_paused_ == paused) {
        return;
    }
    overlay_paused_ = paused;
    if (paused) {
        app_->OnPause();
    } else {
        app_->OnResume();
    }
}
