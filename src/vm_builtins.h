#pragma once

#include "vm.h"

typedef RValue (*BuiltinFunc)(VMContext *ctx, RValue *args, int32_t argCount);

void VMBuiltins_registerAll(void);

BuiltinFunc VMBuiltins_find(const char *name);

RValue VMBuiltins_getVariable(VMContext *ctx, Variable* varDef, int32_t arrayIndex);

void VMBuiltins_setVariable(VMContext *ctx, const char *name, RValue val, int32_t arrayIndex);


typedef enum {
    B_X, B_Y, B_XPREVIOUS, B_YPREVIOUS, B_XSTART, B_YSTART, B_DIRECTION, B_SPEED, B_HSPEED, B_VSPEED, B_FRICTION, B_GRAVITY, B_GRAVITY_DIRECTION,
    B_SPRITE_INDEX, B_IMAGE_INDEX, B_IMAGE_SPEED, B_IMAGE_XSCALE, B_IMAGE_YSCALE,
    B_IMAGE_ANGLE, B_IMAGE_ALPHA, B_IMAGE_BLEND, B_IMAGE_NUMBER,
    B_SPRITE_WIDTH, B_SPRITE_HEIGHT,
    B_VISIBLE, B_DEPTH, B_PERSISTENT, B_SOLID, B_MASK_INDEX, B_ID, B_OBJECT_INDEX, B_ALARM,
    B_BBOX_LEFT, B_BBOX_RIGHT, B_BBOX_TOP, B_BBOX_BOTTOM,
    B_PATH_INDEX, B_PATH_POSITION, B_PATH_POSITIONPREVIOUS, B_PATH_SPEED, B_PATH_SCALE, B_PATH_ORIENTATION,
    B_PATH_ENDACTION,
    B_ROOM, B_ROOM_SPEED, B_ROOM_WIDTH, B_ROOM_HEIGHT, B_ROOM_PERSISTENT,
    B_VIEW_CURRENT, B_VIEW_XVIEW, B_VIEW_YVIEW, B_VIEW_WVIEW, B_VIEW_HVIEW, B_VIEW_VISIBLE, B_VIEW_ANGLE,
    B_VIEW_HBORDER, B_VIEW_VBORDER, B_VIEW_OBJECT,
    B_BG_VISIBLE, B_BG_INDEX, B_BG_X, B_BG_Y, B_BG_HSPEED, B_BG_VSPEED, B_BG_WIDTH, B_BG_HEIGHT, B_BG_ALPHA, B_BG_COLOR,
    B_CURRENT_TIME, B_FPS, B_ARGUMENT_COUNT, B_ARGUMENT,
    B_OS_TYPE, B_OS_WINDOWS, B_OS_PS4, B_OS_PSVITA, B_OS_3DS, B_OS_SWITCH,
    B_WORKING_DIRECTORY, B_MOUSE_X, B_MOUSE_Y, B_KEYBOARD_KEY, B_APP_SURFACE,
    B_TRUE, B_FALSE, B_PI, B_UNDEFINED,
    B_ARGUMENT0,
    B_ARGUMENT1,B_ARGUMENT2,B_ARGUMENT3,B_ARGUMENT4,B_ARGUMENT5,
    B_ARGUMENT6,B_ARGUMENT7,B_ARGUMENT8,B_ARGUMENT9,B_ARGUMENT10,
    B_ARGUMENT11,B_ARGUMENT12,B_ARGUMENT13,B_ARGUMENT14,B_ARGUMENT15,
    B_PATH_ACTION_STOP,
    B_PATH_ACTION_RESTART,
    B_PATH_ACTION_CONTINUE,
    B_PATH_ACTION_REVERSE,
    B_BUILTIN_COUNT,
    B_UNKNOWN = -1
} BuiltinId;

static struct {
    const char *name;
    BuiltinId id;
} builtin_lookup_table[] = {
    {"x", B_X}, {"y", B_Y}, {"xprevious", B_XPREVIOUS}, {"yprevious", B_YPREVIOUS}, {"xstart", B_XSTART}, {"ystart", B_YSTART},
    {"direction", B_DIRECTION}, {"speed", B_SPEED},
    {"hspeed", B_HSPEED}, {"vspeed", B_VSPEED}, {"friction", B_FRICTION}, {"gravity", B_GRAVITY},
    {"gravity_direction", B_GRAVITY_DIRECTION}, {"sprite_index", B_SPRITE_INDEX},
    {"image_index", B_IMAGE_INDEX}, {"image_speed", B_IMAGE_SPEED},
    {"image_xscale", B_IMAGE_XSCALE}, {"image_yscale", B_IMAGE_YSCALE},
    {"image_angle", B_IMAGE_ANGLE}, {"image_alpha", B_IMAGE_ALPHA},
    {"image_blend", B_IMAGE_BLEND}, {"image_number", B_IMAGE_NUMBER},
    {"sprite_width", B_SPRITE_WIDTH}, {"sprite_height", B_SPRITE_HEIGHT},
    {"visible", B_VISIBLE}, {"depth", B_DEPTH}, {"persistent", B_PERSISTENT},
    {"solid", B_SOLID}, {"mask_index", B_MASK_INDEX}, {"id", B_ID},
    {"object_index", B_OBJECT_INDEX}, {"alarm", B_ALARM},
    {"bbox_left", B_BBOX_LEFT}, {"bbox_right", B_BBOX_RIGHT}, {"bbox_top", B_BBOX_TOP}, {"bbox_bottom", B_BBOX_BOTTOM},
    {"path_index", B_PATH_INDEX}, {"path_position", B_PATH_POSITION},
    {"path_positionprevious", B_PATH_POSITIONPREVIOUS},
    {"path_speed", B_PATH_SPEED}, {"path_scale", B_PATH_SCALE}, {"path_orientation", B_PATH_ORIENTATION},
    {"path_endaction", B_PATH_ENDACTION},
    {"room", B_ROOM}, {"room_speed", B_ROOM_SPEED}, {"room_width", B_ROOM_WIDTH}, {"room_height", B_ROOM_HEIGHT},
    {"room_persistent", B_ROOM_PERSISTENT},
    {"view_current", B_VIEW_CURRENT}, {"view_xview", B_VIEW_XVIEW}, {"view_yview", B_VIEW_YVIEW},
    {"view_wview", B_VIEW_WVIEW}, {"view_hview", B_VIEW_HVIEW},
    {"view_visible", B_VIEW_VISIBLE}, {"view_angle", B_VIEW_ANGLE}, {"view_hborder", B_VIEW_HBORDER},
    {"view_vborder", B_VIEW_VBORDER}, {"view_object", B_VIEW_OBJECT},
    {"background_visible", B_BG_VISIBLE}, {"background_index", B_BG_INDEX}, {"background_x", B_BG_X},
    {"background_y", B_BG_Y},
    {"background_hspeed", B_BG_HSPEED}, {"background_vspeed", B_BG_VSPEED}, {"background_width", B_BG_WIDTH},
    {"background_height", B_BG_HEIGHT},
    {"background_alpha", B_BG_ALPHA}, {"background_color", B_BG_COLOR}, {"background_colour", B_BG_COLOR},
    {"current_time", B_CURRENT_TIME}, {"fps", B_FPS},
    {"argument_count", B_ARGUMENT_COUNT}, {"argument", B_ARGUMENT},
    {"argument0", B_ARGUMENT0}, {"argument1", B_ARGUMENT1}, {"argument2", B_ARGUMENT2}, {"argument3", B_ARGUMENT3},
    {"argument4", B_ARGUMENT4}, {"argument5", B_ARGUMENT5}, {"argument6", B_ARGUMENT6}, {"argument7", B_ARGUMENT7},
    {"argument8", B_ARGUMENT8}, {"argument9", B_ARGUMENT9}, {"argument10", B_ARGUMENT10}, {"argument11", B_ARGUMENT11},
    {"argument12", B_ARGUMENT12}, {"argument13", B_ARGUMENT13}, {"argument14", B_ARGUMENT14}, {"argument15", B_ARGUMENT15},
    {"os_type", B_OS_TYPE}, {"os_windows", B_OS_WINDOWS}, {"os_3ds", B_OS_3DS},
    {"working_directory", B_WORKING_DIRECTORY},
    {"mouse_x", B_MOUSE_X}, {"mouse_y", B_MOUSE_Y}, {"keyboard_key", B_KEYBOARD_KEY},{"keyboard_lastkey", B_KEYBOARD_KEY},
    {"application_surface", B_APP_SURFACE},
    {"path_action_stop", B_PATH_ACTION_STOP},
    {"path_action_restart", B_PATH_ACTION_RESTART},
    {"path_action_continue", B_PATH_ACTION_CONTINUE},
    {"path_action_reverse", B_PATH_ACTION_REVERSE},
    {"true", B_TRUE}, {"false", B_FALSE}, {"pi", B_PI}, {"undefined", B_UNDEFINED}
};
