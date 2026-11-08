// Comments panel shared by the watch page and the Shorts player: a header with the count and the
// sort order, comment threads with paging, and a replies view for one thread.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../core/models.h"
#include "../vita/input.h"
#include "gfx.h"
#include "widgets.h"

namespace vt {

class CommentsPanel {
public:
    // continuation: the first page's token (from the watch info); sorts: "Top" / "Newest".
    void start(const std::string& continuation, const std::vector<SortOption>& sorts, const std::string& count);
    void clear();
    bool started() const { return started_; }

    // Handles the d-pad while focused. Up from the header and Left are left unconsumed.
    void update(Input& in, const Rect& r, bool focused);
    void draw(const Rect& r, bool focused);

    bool inReplies() const { return replies_; }
    void closeReplies();
    bool headerFocused() const { return header_; }

private:
    struct Thread {
        std::vector<Comment> items;
        ListView list;
        bool loading = false;
        std::string error, cont;
    };
    void load(bool more);
    void openReplies(const Comment& c);
    void sortMenu();
    float rowHeight(const Comment& c, float w) const;
    void drawRow(const Comment& c, const Rect& row, bool sel, bool parent);
    Thread& cur() { return replies_ ? rep_ : top_; }
    const Thread& cur() const { return replies_ ? rep_ : top_; }

    Thread top_, rep_;
    Comment parent_;
    bool replies_ = false, header_ = false, started_ = false;
    std::vector<SortOption> sorts_;
    int sort_ = 0;
    std::string first_, count_;
    uint32_t gen_ = 0;
    float width_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace vt
