#include <puse/ui/Renderer.hpp>

#include <puse/io/DataLoader.hpp>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace puse::ui {
namespace {

constexpr int kWidth = 480;
constexpr int kHeight = 272;
constexpr int kColumns = 60;
constexpr int kRows = 30;
constexpr int kCellWidth = 8;
constexpr int kCellHeight = 9;
constexpr int kFirstGlyph = 32;
constexpr int kLastGlyph = 126;

struct Glyph {
    SDL_Texture *texture = nullptr;
    int width = 0;
    int height = 0;
};

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
TTF_Font *g_regular = nullptr;
TTF_Font *g_semibold = nullptr;
std::array<Glyph, kLastGlyph - kFirstGlyph + 1> g_regular_glyphs{};
std::array<Glyph, kLastGlyph - kFirstGlyph + 1> g_semibold_glyphs{};
uint32_t g_text_color = 0x00FFFFFF;
uint32_t g_back_color = 0x00000000;
bool g_background_enabled = true;
bool g_bold = false;
int g_cursor_x = 0;
int g_cursor_y = 0;

SDL_Color ToColor(const uint32_t color) {
    return {
        static_cast<uint8_t>(color & 0xFFU),
        static_cast<uint8_t>((color >> 8U) & 0xFFU),
        static_cast<uint8_t>((color >> 16U) & 0xFFU),
        0xFFU,
    };
}

void SetDrawColor(const uint32_t color) {
    const SDL_Color converted = ToColor(color);
    SDL_SetRenderDrawColor(g_renderer, converted.r, converted.g, converted.b, converted.a);
}

void DestroyGlyphs(std::array<Glyph, kLastGlyph - kFirstGlyph + 1> *glyphs) {
    for (auto &glyph : *glyphs) {
        if (glyph.texture != nullptr) SDL_DestroyTexture(glyph.texture);
        glyph = {};
    }
}

bool BuildGlyphs(TTF_Font *font,
                 std::array<Glyph, kLastGlyph - kFirstGlyph + 1> *glyphs) {
    const SDL_Color white{255, 255, 255, 255};
    for (int code = kFirstGlyph; code <= kLastGlyph; ++code) {
        SDL_Surface *surface = TTF_RenderGlyph_Blended(font, static_cast<Uint16>(code), white);
        if (surface == nullptr) return false;
        Glyph &glyph = (*glyphs)[static_cast<size_t>(code - kFirstGlyph)];
        glyph.texture = SDL_CreateTextureFromSurface(g_renderer, surface);
        glyph.width = surface->w;
        glyph.height = surface->h;
        SDL_FreeSurface(surface);
        if (glyph.texture == nullptr) return false;
        SDL_SetTextureBlendMode(glyph.texture, SDL_BLENDMODE_BLEND);
    }
    return true;
}

void Advance() {
    ++g_cursor_x;
    if (g_cursor_x >= kColumns) {
        g_cursor_x = 0;
        ++g_cursor_y;
    }
    if (g_cursor_y >= kRows) g_cursor_y = kRows - 1;
}

void DrawGlyph(const char character) {
    const SDL_Rect cell{g_cursor_x * kCellWidth, g_cursor_y * kCellHeight,
                        kCellWidth, kCellHeight};
    if (g_background_enabled) {
        SetDrawColor(g_back_color);
        SDL_RenderFillRect(g_renderer, &cell);
    }
    const int code = static_cast<unsigned char>(character);
    if (code >= kFirstGlyph && code <= kLastGlyph && character != ' ') {
        auto &glyphs = g_bold ? g_semibold_glyphs : g_regular_glyphs;
        Glyph &glyph = glyphs[static_cast<size_t>(code - kFirstGlyph)];
        const SDL_Color text = ToColor(g_text_color);
        SDL_SetTextureColorMod(glyph.texture, text.r, text.g, text.b);
        const int target_height = kCellHeight - 1;
        const int target_width = std::clamp(
            glyph.height == 0 ? kCellWidth - 1 : glyph.width * target_height / glyph.height,
            1, kCellWidth);
        const SDL_Rect target{
            cell.x + (kCellWidth - target_width) / 2,
            cell.y,
            target_width,
            target_height,
        };
        SDL_RenderCopy(g_renderer, glyph.texture, nullptr, &target);
    }
    Advance();
}

void DrawBadge(const char label, const uint32_t color) {
    const int x = g_cursor_x * kCellWidth + 1;
    const int y = g_cursor_y * kCellHeight;
    const SDL_Rect badge{x, y, kCellWidth * 2 + 4, kCellHeight};
    SetDrawColor(color);
    SDL_RenderFillRect(g_renderer, &badge);
    const uint32_t previous_text = g_text_color;
    const bool previous_bold = g_bold;
    const bool previous_background = g_background_enabled;
    g_text_color = 0x00101412;
    g_bold = true;
    g_background_enabled = false;
    ++g_cursor_x;
    DrawGlyph(label);
    ++g_cursor_x;
    g_text_color = previous_text;
    g_bold = previous_bold;
    g_background_enabled = previous_background;
}

bool Match(const std::string &text, const size_t offset, const char *token) {
    return text.compare(offset, std::char_traits<char>::length(token), token) == 0;
}

void DrawText(const std::string &text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (Match(text, i, "[O]")) {
            DrawBadge('O', 0x0091E82A);
            i += 2;
            continue;
        }
        if (Match(text, i, "[X]")) {
            DrawBadge('X', 0x005B5BF5);
            i += 2;
            continue;
        }
        const char character = text[i];
        if (character == '\n') {
            g_cursor_x = 0;
            g_cursor_y = std::min(kRows - 1, g_cursor_y + 1);
        } else if (character == '\r') {
            g_cursor_x = 0;
        } else {
            DrawGlyph(character);
        }
    }
    SDL_RenderPresent(g_renderer);
}

} // namespace

bool Init() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) return false;
    g_window = SDL_CreateWindow("PUSE-PSP", SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED, kWidth, kHeight, 0);
    if (g_window == nullptr) return false;
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (g_renderer == nullptr) g_renderer = SDL_CreateRenderer(g_window, -1, 0);
    if (g_renderer == nullptr) return false;
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);

    const std::string regular_path = puse::io::ResolveAssetPath("data/ui/SourceCodePro-Regular.ttf");
    const std::string semibold_path = puse::io::ResolveAssetPath("data/ui/SourceCodePro-Semibold.ttf");
    g_regular = TTF_OpenFont(regular_path.c_str(), 12);
    g_semibold = TTF_OpenFont(semibold_path.c_str(), 12);
    if (g_regular == nullptr || g_semibold == nullptr) return false;
    TTF_SetFontHinting(g_regular, TTF_HINTING_MONO);
    TTF_SetFontHinting(g_semibold, TTF_HINTING_MONO);
    if (!BuildGlyphs(g_regular, &g_regular_glyphs) ||
        !BuildGlyphs(g_semibold, &g_semibold_glyphs)) return false;
    Clear();
    return true;
}

void Shutdown() {
    DestroyGlyphs(&g_regular_glyphs);
    DestroyGlyphs(&g_semibold_glyphs);
    if (g_regular != nullptr) TTF_CloseFont(g_regular);
    if (g_semibold != nullptr) TTF_CloseFont(g_semibold);
    if (g_renderer != nullptr) SDL_DestroyRenderer(g_renderer);
    if (g_window != nullptr) SDL_DestroyWindow(g_window);
    g_regular = nullptr;
    g_semibold = nullptr;
    g_renderer = nullptr;
    g_window = nullptr;
    TTF_Quit();
    SDL_Quit();
}

void Clear() {
    if (g_renderer == nullptr) return;
    SetDrawColor(g_back_color);
    SDL_RenderClear(g_renderer);
    g_cursor_x = 0;
    g_cursor_y = 0;
    SDL_RenderPresent(g_renderer);
}

void SetTextColor(const uint32_t color) { g_text_color = color; }
void SetBackColor(const uint32_t color) { g_back_color = color; }
void EnableBackColor(const int enable) { g_background_enabled = enable != 0; }
void SetXY(const int x, const int y) {
    g_cursor_x = std::clamp(x, 0, kColumns - 1);
    g_cursor_y = std::clamp(y, 0, kRows - 1);
}
int GetX() { return g_cursor_x; }
int GetY() { return g_cursor_y; }
void SetBold(const bool bold) { g_bold = bold; }

int Printf(const char *format, ...) {
    std::array<char, 4096> buffer{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer.data(), buffer.size(), format, args);
    va_end(args);
    DrawText(buffer.data());
    return written;
}

} // namespace puse::ui
