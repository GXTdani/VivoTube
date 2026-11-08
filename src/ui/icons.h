// Small vector icons drawn with the Gfx shape primitives (no image assets).
#pragma once

#include <cstdint>

#include "gfx.h"

namespace vt {

enum class Icon {
    Home,
    HomeFilled,
    Subscriptions,
    SubscriptionsFilled,
    You,
    YouFilled,
    Search,
    Settings,
    History,
    WatchLater,
    Back,
    More,
    Play,
    Pause,
    Close,
    Check,
    Save,
    Captions,
    Fullscreen,
    Playlist,
    Next,
    ArrowUpLeft,
    Comment,
    Info,
    Shuffle,
    Trash,
    ChevronRight,
    Menu,
    Shorts,
    ShortsFilled,
    ThumbUp,
    ThumbUpFilled,
    ThumbDown,
    ThumbDownFilled,
    Share,
    Account,
    Filter,
    Sort,
    Loop,
    Plus,
    Lock,
    SignOut,
    Download,
    Verified,
    Liked,
    ChevronDown,
    Refresh,
};

// Draws an icon centered at (cx, cy), `size` pixels square. `bg` fills cut-outs in filled icons.
void draw_icon(Icon icon, float cx, float cy, float size, uint32_t color, uint32_t bg = col::bg);

// VivoTube's own mark: a rounded teal-blue tile with a white ring and a coral V.
void draw_brand_mark(float x, float y, float size, uint32_t bg = col::bg);

}  // namespace vt
