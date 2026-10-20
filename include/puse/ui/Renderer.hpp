#pragma once

#include <cstdint>

namespace puse::ui {

bool Init();
void Shutdown();
void Clear();
void SetTextColor(uint32_t color);
void SetBackColor(uint32_t color);
void EnableBackColor(int enable);
void SetXY(int x, int y);
int GetX();
int GetY();
void SetBold(bool bold);
int Printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace puse::ui
