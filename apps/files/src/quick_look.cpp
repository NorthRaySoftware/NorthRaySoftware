// NorthRay Files: quick look (Space, 0.8). The focused file large over the window: a picture at full size
// (Windows' own decoders, or the art and Shell pictures the thumbnails make), the start of a text file, or its
// big icon, with its name, size, date and pixel size. Space or Esc closes it; the arrow keys move to the next
// or previous file and the look follows; Enter opens it.
#include "files_window.hpp"

#include "nrf/paths.hpp"

#include "cjui/gfx.hpp"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace nrf {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::Rect;

// A picture's width and height from its header (Windows Imaging Component; nothing is decoded). Kept per path.
std::pair<unsigned, unsigned> FilesWindow::pictureSize(const std::wstring& path) const {
    auto it = pictureSizes_.find(path);
    if (it != pictureSizes_.end()) return it->second;
    std::pair<unsigned, unsigned> wh{0, 0};
    Microsoft::WRL::ComPtr<IWICImagingFactory> f;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> d;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> fr;
        UINT w = 0, h = 0;
        if (SUCCEEDED(f->CreateDecoderFromFilename(extendedPath(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
            SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&w, &h)))
            wh = {w, h};
    }
    if (pictureSizes_.size() > 2000) pictureSizes_.clear();
    pictureSizes_[path] = wh;
    return wh;
}

void FilesWindow::openQuickLook() {
    if (here_.thisPC || shown_.empty()) return;
    if (sel_.indices().empty()) return;
    quickLook_ = true;
    invalidate();
}

bool FilesWindow::quickLookKey(WPARAM vk) {
    switch (vk) {
        case VK_SPACE:
        case VK_ESCAPE: quickLook_ = false; invalidate(); return true;
        case VK_RETURN: quickLook_ = false; openSelection(); return true;
        default: return false;   // the arrows move the selection as usual, and the look follows
    }
}

void FilesWindow::drawQuickLook(float W, float H) {
    const auto rows = sel_.indices();
    if (rows.empty() || here_.thisPC) {
        quickLook_ = false;
        return;
    }
    const int f = sel_.focus();
    const std::size_t row = (f >= 0 && (std::size_t)f < shown_.size() && sel_.selected((std::size_t)f)) ? (std::size_t)f : rows[0];
    const Entry& e = shown_[row];
    const std::wstring path = rowPath(row);
    gfx_.fill({0, 0, W, H}, cjui::hexColor("#000000").withAlpha(0.72f));
    const Rect card{24, 24, W - 48, H - 48};
    quickCard_ = card;
    gfx_.fillRounded(card, 12, pal_.surface);
    // Its name and details along the top.
    gfx_.text(e.name, {card.x + 20, card.y + 14, card.w - 260, 26}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
    std::wstring info;
    if (!e.folder()) info = formatSize(e.size);
    const std::wstring d = dateText(e.modified);
    if (!d.empty()) info += (info.empty() ? L"" : L"  ·  ") + d;
    const bool canPicture = !e.folder() && !e.cloudOnly() && Thumbnails::hasPicture(e.name);
    if (canPicture) {
        const auto px = pictureSize(path);
        if (px.first) info += L"  ·  " + std::to_wstring(px.first) + L" × " + std::to_wstring(px.second);
    }
    gfx_.text(info, {card.x + 20, card.y + 42, card.w - 40, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    gfx_.text(L"Space closes · arrows for the next · Enter opens", {card.right() - 340, card.y + 16, 320, 20}, Font::Small, pal_.inkFaint,
              {Align::Right, false, true, true});
    const Rect box{card.x + 20, card.y + 74, card.w - 40, card.h - 94};
    bool drawn = false;
    // A picture: Windows decodes the real file at full size where it can; art and Shell kinds use the 512 px picture.
    if (canPicture) {
        if (Thumbnails::kindOf(e.name) == Thumbnails::PictureKind::Image && e.size < 200ull * 1024 * 1024)
            drawn = gfx_.drawImage(extendedPath(path), box, false);
        if (!drawn && thumbs_) {
            if (const Thumb* th = thumbs_->get(path, e.modified, e.size, 512); th && th->w > 0 && th->h > 0) {
                const float s = std::min(box.w / th->w, box.h / th->h);
                const float pw = th->w * s, ph = th->h * s;
                const Rect p{std::round(box.x + (box.w - pw) / 2), std::round(box.y + (box.h - ph) / 2), std::round(pw), std::round(ph)};
                gfx_.drawPixels(L"thumb:" + std::to_wstring(th->serial), th->w, th->h, th->rgba.data(), p, true);
                drawn = true;
            }
        }
    }
    // Text: the same reader as the preview pane.
    if (!drawn && !e.folder() && !e.cloudOnly() && isTextName(e.name) && e.size > 0) {
        const std::wstring key = path + L"|" + std::to_wstring(e.modified) + L"|" + std::to_wstring(e.size);
        if (key != previewKey_) requestTextPreview(key, path);
        if (previewState_ == PreviewText) {
            gfx_.fillRounded(box, 8, pal_.paper);
            const Rect tr{box.x + 16, box.y + 12, box.w - 32, box.h - 24};
            gfx_.pushClip(tr);
            gfx_.text(previewText_.substr(0, 20000), tr, Font::Body, pal_.ink, {Align::Left, true});
            gfx_.popClip();
            drawn = true;
        } else if (previewState_ == PreviewLoading) {
            gfx_.text(L"Reading…", box, Font::Body, pal_.inkFaint, {Align::Center, false, false, true});
            drawn = true;
        }
    }
    if (!drawn) {
        drawIcon(e.icon, {box.x + (box.w - 96) / 2, box.y + (box.h - 96) / 2 - 16, 96, 96});
        gfx_.text(e.folder() ? L"A folder: Enter opens it." : canPicture ? L"Making a picture…" : L"No preview for this kind of file. Enter opens it.",
                  {box.x, box.y + box.h / 2 + 50, box.w, 22}, Font::Body, pal_.inkMuted, {Align::Center, false, true, true});
    }
}

}  // namespace nrf
