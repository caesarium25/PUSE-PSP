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
constexpr int kColumns = 40;
constexpr int kRows = 17;
constexpr int kCellWidth = 12;
constexpr int kCellHeight = 16;
constexpr int kFirstGlyph = 32;
constexpr int kLastGlyph = 126;

struct Glyph {
    SDL_Texture *texture = nullptr;
    int width = 0;
    int height = 0;
};

struct Cell {
    char character = ' ';
    uint32_t text_color = 0x00FFFFFF;
    uint32_t back_color = 0x00000000;
    bool background_enabled = true;
    bool bold = false;
};

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
TTF_Font *g_regular = nullptr;
TTF_Font *g_semibold = nullptr;
std::array<Glyph, kLastGlyph - kFirstGlyph + 1> g_regular_glyphs{};
std::array<Glyph, kLastGlyph - kFirstGlyph + 1> g_semibold_glyphs{};
std::array<Cell, kColumns * kRows> g_cells{};
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

void DrawCell(const int x, const int y, const Cell &value) {
    const SDL_Rect bounds{x * kCellWidth, y * kCellHeight, kCellWidth, kCellHeight};
    if (value.background_enabled) {
        SetDrawColor(value.back_color);
        SDL_RenderFillRect(g_renderer, &bounds);
    }
    const int code = static_cast<unsigned char>(value.character);
    if (code >= kFirstGlyph && code <= kLastGlyph && value.character != ' ') {
        auto &glyphs = value.bold ? g_semibold_glyphs : g_regular_glyphs;
        Glyph &glyph = glyphs[static_cast<size_t>(code - kFirstGlyph)];
        const SDL_Color text = ToColor(value.text_color);
        SDL_SetTextureColorMod(glyph.texture, text.r, text.g, text.b);
        // Keep glyphs at their native rasterized size. Scaling small TTF glyphs
        // down to the old 8x9 grid made them muddy on the PSP LCD.
        const SDL_Rect target{
            bounds.x + (kCellWidth - glyph.width) / 2,
            bounds.y + (kCellHeight - glyph.height) / 2,
            glyph.width,
            glyph.height,
        };
        SDL_RenderSetClipRect(g_renderer, &bounds);
        SDL_RenderCopy(g_renderer, glyph.texture, nullptr, &target);
        SDL_RenderSetClipRect(g_renderer, nullptr);
    }
}

void RenderFrame() {
    SetDrawColor(g_back_color);
    SDL_RenderClear(g_renderer);
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kColumns; ++x) {
            DrawCell(x, y, g_cells[static_cast<size_t>(y * kColumns + x)]);
        }
    }
    SDL_RenderPresent(g_renderer);
}

void DrawGlyph(const char character) {
    g_cells[static_cast<size_t>(g_cursor_y * kColumns + g_cursor_x)] = {
        character, g_text_color, g_back_color, g_background_enabled, g_bold};
    Advance();
}

void DrawBadge(const char label, const uint32_t color) {
    for (int offset = 0; offset < 3; ++offset) {
        const char character = offset == 1 ? label : ' ';
        g_cells[static_cast<size_t>(g_cursor_y * kColumns + g_cursor_x)] = {
            character, 0x00101412, color, true, true};
        Advance();
    }
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
    RenderFrame();
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
    g_regular = TTF_OpenFont(regular_path.c_str(), 14);
    g_semibold = TTF_OpenFont(semibold_path.c_str(), 14);
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
    for (auto &cell : g_cells) {
        cell = {' ', g_text_color, g_back_color, true, false};
    }
    g_cursor_x = 0;
    g_cursor_y = 0;
    RenderFrame();
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
