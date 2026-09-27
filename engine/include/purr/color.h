#pragma once

// PurrLang's Color: red, green, blue and alpha from 0 to 1, as in Unity.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. Generated code uses the type and the constants below.

typedef struct purr_color {
    float r, g, b, a;
} purr_color;

// Color.white and friends, with Unity's values.
#define PURR_COLOR_WHITE ((purr_color){1.0f, 1.0f, 1.0f, 1.0f})
#define PURR_COLOR_BLACK ((purr_color){0.0f, 0.0f, 0.0f, 1.0f})
#define PURR_COLOR_RED ((purr_color){1.0f, 0.0f, 0.0f, 1.0f})
#define PURR_COLOR_GREEN ((purr_color){0.0f, 1.0f, 0.0f, 1.0f})
#define PURR_COLOR_BLUE ((purr_color){0.0f, 0.0f, 1.0f, 1.0f})
#define PURR_COLOR_YELLOW ((purr_color){1.0f, 0.92156863f, 0.015686275f, 1.0f})
#define PURR_COLOR_CYAN ((purr_color){0.0f, 1.0f, 1.0f, 1.0f})
#define PURR_COLOR_MAGENTA ((purr_color){1.0f, 0.0f, 1.0f, 1.0f})
#define PURR_COLOR_GRAY ((purr_color){0.5f, 0.5f, 0.5f, 1.0f})
#define PURR_COLOR_CLEAR ((purr_color){0.0f, 0.0f, 0.0f, 0.0f})
