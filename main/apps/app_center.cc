// 应用中心：黑底、无标题栏、每页最多 4 个大图标。
// 1/2 个应用时整行居中，3/4 个时按 2×2 网格整体居中；超过 4 个左右横滑翻页。
#include "app.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <lvgl.h>

extern const AppInfo kAppCenterInfo = {"app_center", "应用中心", 0x000000};

namespace {

constexpr int kDa = 0x8A8F98;
constexpr uint32_t kLabel = 0xFFFFFF;
constexpr uint32_t kDotIdle = 0x3A3F47;
constexpr uint32_t kDotActive = 0xFFFFFF;

constexpr int kScreen = 360;
constexpr int kIcon = 80;
constexpr int kCellW = 100;
constexpr int kCellH = 96;
constexpr int kColX[2] = {112, 248};
constexpr int kRowY2[2] = {114, 238};
constexpr int kRowY1 = 176;

class AppCenterApp;

struct TileTarget {
    AppCenterApp* app;
    AppContext* ctx;
    const char* id;
};

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color, const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

void AnimateX(void* var, int32_t value) {
    lv_obj_set_x(static_cast<lv_obj_t*>(var), value);
}

class AppCenterApp : public App {
public:
    const AppInfo& info() const override { return kAppCenterInfo; }

    void OnEnter(AppContext& ctx) override {
        ctx_ = &ctx;
        root_ = ctx.root();
        lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_EVENT_BUBBLE);

        font_ = ctx.font();
        if (font_ == nullptr) {
            font_ = &lv_font_montserrat_14;
        }

        apps_.clear();
        int count = 0;
        const AppEntry* entries = GetAppRegistry(&count);
        for (int i = 0; i < count; ++i) {
            const AppInfo* info = entries[i].info;
            if (info != nullptr && info->id != nullptr &&
                strcmp(info->id, kAppCenterInfo.id) != 0) {
                apps_.push_back(info);
            }
        }

        if (apps_.empty()) {
            lv_obj_t* empty = MakeLabel(root_, "暂无应用", kDa, font_);
            lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
            return;
        }

        pages_ = static_cast<int>((apps_.size() + 3) / 4);
        page_ = 0;
        BuildPages();
        if (pages_ > 1) {
            BuildDots();
        }

        lv_obj_add_event_cb(root_, OnPressed, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(root_, OnPressing, LV_EVENT_PRESSING, this);
        lv_obj_add_event_cb(root_, OnReleased, LV_EVENT_RELEASED, this);
    }

    void OnExit() override {
        // 这几个回调是挂在 root_ 上的，而 root_ 是 AppHost 复用的应用层对象本身
        // （不是它的子对象）。lv_obj_clean() 只删子对象，删不掉挂在对象身上的回调；
        // 而 AppHost 紧接着会 delete 掉本 AppCenterApp 实例。不在这里主动摘掉，
        // 这些回调就变成指向已释放内存的野指针 —— 下一个应用（手电/电台）触摸屏幕时
        // 触发同一个 layer 的 PRESSED/RELEASED，就会跳进已经析构的 AppCenterApp，
        // 读写野指针，表现为「从应用中心进手电后一碰就死机」。多次进出还会不断叠加。
        if (root_ != nullptr) {
            lv_obj_remove_event_cb(root_, OnPressed);
            lv_obj_remove_event_cb(root_, OnPressing);
            lv_obj_remove_event_cb(root_, OnReleased);
            // 事件冒泡标志同样要还原：它是本应用加上去的，留着会让后续应用的
            // 触摸事件继续往上层桌面冒泡。
            lv_obj_remove_flag(root_, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        if (pages_host_ != nullptr) {
            lv_anim_delete(pages_host_, nullptr);
        }
        pages_host_ = nullptr;
        root_ = nullptr;
        ctx_ = nullptr;
        dots_.clear();
        apps_.clear();
    }

    bool ConsumeSuppressedClick() {
        const bool suppressed = suppress_click_;
        suppress_click_ = false;
        return suppressed;
    }

private:
    void BuildPages() {
        pages_host_ = lv_obj_create(root_);
        lv_obj_remove_style_all(pages_host_);
        lv_obj_set_size(pages_host_, kScreen, kScreen);
        lv_obj_set_pos(pages_host_, 0, 0);
        lv_obj_remove_flag(pages_host_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pages_host_, LV_OBJ_FLAG_EVENT_BUBBLE);

        for (int p = 0; p < pages_; ++p) {
            const int first = p * 4;
            const int n = static_cast<int>(apps_.size()) - first;
            const int shown = n > 4 ? 4 : n;

            lv_obj_t* page = lv_obj_create(pages_host_);
            lv_obj_remove_style_all(page);
            lv_obj_set_size(page, kScreen, kScreen);
            lv_obj_set_pos(page, p * kScreen, 0);
            lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(page, LV_OBJ_FLAG_EVENT_BUBBLE);

            const bool two_rows = shown > 2;
            const int cols = shown == 1 ? 1 : 2;
            for (int i = 0; i < shown; ++i) {
                const int col = i % cols;
                const int row = two_rows ? (i / 2) : 0;
                const int cx = (cols == 1) ? 180 : kColX[col];
                const int cy = two_rows ? kRowY2[row] : kRowY1;
                BuildTile(page, apps_[first + i], cx, cy);
            }
        }
    }

    void BuildTile(lv_obj_t* page, const AppInfo* info, int cx, int cy) {
        lv_obj_t* tile = lv_obj_create(page);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, kCellW, kCellH);
        lv_obj_set_pos(tile, cx - kCellW / 2, cy - 42);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_opa(tile, LV_OPA_70, LV_STATE_PRESSED);

        if (info->draw_icon != nullptr) {
            info->draw_icon(tile, 10, 2);
        } else {
            lv_obj_t* dot = lv_obj_create(tile);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 44, 44);
            lv_obj_set_pos(dot, 28, 20);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(info->accent), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t* name = MakeLabel(tile, info->name, kLabel, font_);
        lv_obj_set_width(name, kCellW);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 70);

        auto* target = new TileTarget{this, ctx_, info->id};
        lv_obj_add_event_cb(tile, OnTileClicked, LV_EVENT_CLICKED, target);
        lv_obj_add_event_cb(tile, OnTileDeleted, LV_EVENT_DELETE, target);
    }

    void BuildDots() {
        const int gap = 8;
        const int total = pages_ * 4 + (pages_ - 1) * gap;
        const int left = 180 - total / 2;
        for (int i = 0; i < pages_; ++i) {
            lv_obj_t* dot = lv_obj_create(root_);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 4, 4);
            lv_obj_set_pos(dot, left + i * (4 + gap), 334);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(i == 0 ? kDotActive : kDotIdle), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            dots_.push_back(dot);
        }
    }

    void UpdateDots() {
        for (int i = 0; i < static_cast<int>(dots_.size()); ++i) {
            lv_obj_set_style_bg_color(dots_[i], lv_color_hex(i == page_ ? kDotActive : kDotIdle), 0);
        }
    }

    void MoveToPage(int page) {
        page_ = page;
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, pages_host_);
        lv_anim_set_exec_cb(&anim, AnimateX);
        lv_anim_set_values(&anim, lv_obj_get_x(pages_host_), -page_ * kScreen);
        lv_anim_set_duration(&anim, 220);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_start(&anim);
        UpdateDots();
    }

    int PointerX(lv_event_t* e) {
        lv_indev_t* indev = lv_event_get_indev(e);
        lv_point_t point = {};
        if (indev != nullptr) {
            lv_indev_get_point(indev, &point);
        }
        return point.x;
    }

    static void OnPressed(lv_event_t* e) {
        auto* self = static_cast<AppCenterApp*>(lv_event_get_user_data(e));
        self->press_x_ = self->PointerX(e);
        self->drag_dx_ = 0;
        self->dragging_ = false;
        self->suppress_click_ = false;
    }

    static void OnPressing(lv_event_t* e) {
        auto* self = static_cast<AppCenterApp*>(lv_event_get_user_data(e));
        const int dx = self->PointerX(e) - self->press_x_;
        self->drag_dx_ = dx;
        if (!self->dragging_ && (dx > 12 || dx < -12)) {
            self->dragging_ = true;
        }
        if (!self->dragging_) {
            return;
        }
        int shift = dx;
        if ((self->page_ == 0 && dx > 0) || (self->page_ >= self->pages_ - 1 && dx < 0)) {
            shift = dx / 3;
        }
        lv_obj_set_x(self->pages_host_, -self->page_ * kScreen + shift);
    }

    static void OnReleased(lv_event_t* e) {
        auto* self = static_cast<AppCenterApp*>(lv_event_get_user_data(e));
        if (!self->dragging_) {
            return;
        }
        int page = self->page_;
        if (self->drag_dx_ < -40 && page < self->pages_ - 1) {
            ++page;
        } else if (self->drag_dx_ > 40 && page > 0) {
            --page;
        }
        self->suppress_click_ = true;
        self->dragging_ = false;
        self->MoveToPage(page);
    }

    static void OnTileClicked(lv_event_t* e) {
        auto* target = static_cast<TileTarget*>(lv_event_get_user_data(e));
        if (target == nullptr || target->app == nullptr || target->id == nullptr) {
            return;
        }
        if (target->app->ConsumeSuppressedClick()) {
            return;
        }
        target->ctx->SwitchTo(target->id);
    }

    static void OnTileDeleted(lv_event_t* e) {
        delete static_cast<TileTarget*>(lv_event_get_user_data(e));
    }

    AppContext* ctx_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* pages_host_ = nullptr;
    const lv_font_t* font_ = nullptr;
    std::vector<const AppInfo*> apps_;
    std::vector<lv_obj_t*> dots_;
    int pages_ = 0;
    int page_ = 0;
    int press_x_ = 0;
    int drag_dx_ = 0;
    bool dragging_ = false;
    bool suppress_click_ = false;
};

}  // namespace

App* CreateAppCenterApp() {
    return new AppCenterApp();
}
