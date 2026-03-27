#include "runner_keyboard.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

static bool isValidKey(int32_t key) {
    return key >= 0 && GML_KEY_COUNT > key;
}

static bool isValidMouseBtn(int32_t btn) {
    // GML: mb_left=1, mb_right=2, mb_middle=3
    return btn >= 1 && btn < GML_MOUSE_BUTTON_COUNT;
}

RunnerKeyboardState* RunnerKeyboard_create(void) {
    RunnerKeyboardState* kb = safeCalloc(1, sizeof(RunnerKeyboardState));
    kb->lastKey = VK_NOKEY;
    return kb;
}

void RunnerKeyboard_free(RunnerKeyboardState* kb) {
    free(kb);
}

void RunnerKeyboard_beginFrame(RunnerKeyboardState* kb) {
    memset(kb->keyPressed,   0, sizeof(kb->keyPressed));
    memset(kb->keyReleased,  0, sizeof(kb->keyReleased));
    // Сбрасываем per-frame мышиные события (down/up остаются!)
    memset(kb->mousePressed,  0, sizeof(kb->mousePressed));
    memset(kb->mouseReleased, 0, sizeof(kb->mouseReleased));
}

void RunnerKeyboard_onKeyDown(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode]    = true;
    kb->keyPressed[gmlKeyCode] = true;
    kb->lastKey = gmlKeyCode;
}

void RunnerKeyboard_onKeyUp(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode]     = false;
    kb->keyReleased[gmlKeyCode] = true;
}

// ===[ Mouse ]===

void RunnerKeyboard_onMouseDown(RunnerKeyboardState* kb, int32_t btn) {
    if (!isValidMouseBtn(btn)) return;
    kb->mouseDown[btn]    = true;
    kb->mousePressed[btn] = true;
}

void RunnerKeyboard_onMouseUp(RunnerKeyboardState* kb, int32_t btn) {
    if (!isValidMouseBtn(btn)) return;
    kb->mouseDown[btn]     = false;
    kb->mouseReleased[btn] = true;
}

bool RunnerKeyboard_mouseCheck(RunnerKeyboardState* kb, int32_t btn) {
    if (!isValidMouseBtn(btn)) return false;
    return kb->mouseDown[btn];
}

bool RunnerKeyboard_mouseCheckPressed(RunnerKeyboardState* kb, int32_t btn) {
    if (!isValidMouseBtn(btn)) return false;
    return kb->mousePressed[btn];
}

bool RunnerKeyboard_mouseCheckReleased(RunnerKeyboardState* kb, int32_t btn) {
    if (!isValidMouseBtn(btn)) return false;
    return kb->mouseReleased[btn];
}

// ===[ Keyboard checks (без изменений) ]===

bool RunnerKeyboard_check(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (gmlKeyCode == VK_ANYKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyDown[i]) return true;
        }
        return false;
    }
    if (gmlKeyCode == VK_NOKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyDown[i]) return false;
        }
        return true;
    }
    if (!isValidKey(gmlKeyCode)) return false;
    return kb->keyDown[gmlKeyCode];
}

bool RunnerKeyboard_checkPressed(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (gmlKeyCode == VK_ANYKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyPressed[i]) return true;
        }
        return false;
    }
    if (gmlKeyCode == VK_NOKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyPressed[i]) return false;
        }
        return true;
    }
    if (!isValidKey(gmlKeyCode)) return false;
    return kb->keyPressed[gmlKeyCode];
}

bool RunnerKeyboard_checkReleased(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (gmlKeyCode == VK_ANYKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyReleased[i]) return true;
        }
        return false;
    }
    if (gmlKeyCode == VK_NOKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyReleased[i]) return false;
        }
        return true;
    }
    if (!isValidKey(gmlKeyCode)) return false;
    return kb->keyReleased[gmlKeyCode];
}

void RunnerKeyboard_simulatePress(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode]    = true;
    kb->keyPressed[gmlKeyCode] = true;
    kb->lastKey = gmlKeyCode;
}

void RunnerKeyboard_simulateRelease(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode]     = false;
    kb->keyReleased[gmlKeyCode] = true;
}

void RunnerKeyboard_clear(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (gmlKeyCode == VK_ANYKEY) {
        memset(kb->keyDown,    0, sizeof(kb->keyDown));
        memset(kb->keyPressed, 0, sizeof(kb->keyPressed));
        memset(kb->keyReleased,0, sizeof(kb->keyReleased));
        // Мышь тоже сбрасываем при VK_ANYKEY
        memset(kb->mouseDown,    0, sizeof(kb->mouseDown));
        memset(kb->mousePressed, 0, sizeof(kb->mousePressed));
        memset(kb->mouseReleased,0, sizeof(kb->mouseReleased));
        kb->lastKey = VK_NOKEY;
        return;
    }
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode]    = false;
    kb->keyPressed[gmlKeyCode] = false;
    kb->keyReleased[gmlKeyCode]= false;
}