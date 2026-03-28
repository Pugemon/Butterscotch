//
// Created by Pugemon on 27.03.2026.
//

#pragma once

#include "renderer.h"

#ifdef __3DS__
// Создает экземпляр рендерера, использующего аппаратное ускорение 3DS (Citro3D)
Renderer* Citro3dRenderer_create(void);
#endif