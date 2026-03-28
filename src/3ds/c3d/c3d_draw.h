#pragma once
#ifdef __3DS__

#include "renderer.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @file c3d_draw.h
 * @brief Высокоуровневые draw-функции рендерера Citro3D.
 *
 * Каждая функция превращает игровые координаты в пары треугольников
 * (2 треугольника = 6 вершин = 1 quad) и записывает их в VBO через c3d_batch.
 *
 * ## Система координат
 *
 * После C3DRenderer_beginView:
 *  - (0, 0) = левый верхний угол view-области
 *  - X растёт вправо, Y растёт вниз
 *  - Z фиксирован = SPRITE_Z_DEPTH (глубина не используется)
 *
 * ## Треугольники
 *
 * Один спрайт = 2 треугольника без дублирования вершин (нет index buffer):
 *
 *   v0 ─── v1
 *   │  ╲   │
 *   │   ╲  │
 *   v2 ─── v3
 *
 *   Triangle 1: v0, v1, v2
 *   Triangle 2: v1, v3, v2
 */

/**
 * @brief Рисует спрайт с полным набором трансформаций.
 *
 * @param renderer    Рендерер
 * @param tpagIndex   Индекс TexturePageItem в DataWin->tpag
 * @param x, y        Позиция в мировых координатах
 * @param originX/Y   Точка трансформации (pivot) в координатах спрайта
 * @param xscale/yscale Масштаб
 * @param angleDeg    Угол поворота (градусы, по часовой стрелке в экранных координатах)
 * @param color       Цвет-тинт в формате 0x00BBGGRR (BGR)
 * @param alpha       Прозрачность [0.0, 1.0]
 */
void C3DRenderer_drawSprite(Renderer *renderer,
                             int32_t tpagIndex,
                             float x, float y,
                             float originX, float originY,
                             float xscale, float yscale,
                             float angleDeg,
                             uint32_t color, float alpha);

/**
 * @brief Рисует прямоугольную часть спрайта (без поворота и pivot).
 *
 * @param srcOffX/Y  Смещение в атласе относительно начала TPAG (пикселей)
 * @param srcW/H     Размер вырезаемого прямоугольника
 */
void C3DRenderer_drawSpritePart(Renderer *renderer,
                                 int32_t tpagIndex,
                                 int32_t srcOffX, int32_t srcOffY,
                                 int32_t srcW, int32_t srcH,
                                 float x, float y,
                                 float xscale, float yscale,
                                 uint32_t color, float alpha);

/**
 * @brief Рисует закрашенный или обведённый прямоугольник.
 *
 * Использует белую текстуру; итоговый цвет = color * alpha.
 *
 * @param outline true — только контур (4 линии), false — заливка
 */
void C3DRenderer_drawRectangle(Renderer *renderer,
                                float x1, float y1, float x2, float y2,
                                uint32_t color, float alpha, bool outline);

/**
 * @brief Рисует линию заданной толщины.
 *
 * Линия реализована как повёрнутый прямоугольник (quad) с шириной width.
 * Если длина линии < LINE_MIN_LENGTH — вызов игнорируется.
 */
void C3DRenderer_drawLine(Renderer *renderer,
                           float x1, float y1, float x2, float y2,
                           float width, uint32_t color, float alpha);

/**
 * @brief Рисует линию с градиентом цвета (упрощённая версия — использует color1).
 *
 * TODO: реализовать настоящий градиент через разные цвета вершин.
 */
void C3DRenderer_drawLineColor(Renderer *renderer,
                                float x1, float y1, float x2, float y2,
                                float width,
                                uint32_t color1, uint32_t color2, float alpha);

/**
 * @brief Рисует текст с использованием шрифта DataWin.
 *
 * Поддерживает UTF-8, кернинг и перенос строк ('\n').
 * Каждый глиф рисуется через C3DRenderer_drawSpritePart.
 *
 * @param angleDeg Поворот (пока не реализован, игнорируется)
 */
void C3DRenderer_drawText(Renderer *renderer,
                           const char *text,
                           float x, float y,
                           float xscale, float yscale,
                           float angleDeg);

#endif // __3DS__
