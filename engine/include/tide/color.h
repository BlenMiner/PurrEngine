#pragma once

// Tide's Color: red, green, blue and alpha from 0 to 1, as in Unity.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. Generated code uses the type and the constants below.

typedef struct tide_color {
    float r, g, b, a;
} tide_color;

// Color.white and friends, with Unity's values.
#define TIDE_COLOR_WHITE ((tide_color){1.0f, 1.0f, 1.0f, 1.0f})
#define TIDE_COLOR_BLACK ((tide_color){0.0f, 0.0f, 0.0f, 1.0f})
#define TIDE_COLOR_RED ((tide_color){1.0f, 0.0f, 0.0f, 1.0f})
#define TIDE_COLOR_GREEN ((tide_color){0.0f, 1.0f, 0.0f, 1.0f})
#define TIDE_COLOR_BLUE ((tide_color){0.0f, 0.0f, 1.0f, 1.0f})
#define TIDE_COLOR_YELLOW ((tide_color){1.0f, 0.92156863f, 0.015686275f, 1.0f})
#define TIDE_COLOR_CYAN ((tide_color){0.0f, 1.0f, 1.0f, 1.0f})
#define TIDE_COLOR_MAGENTA ((tide_color){1.0f, 0.0f, 1.0f, 1.0f})
#define TIDE_COLOR_GRAY ((tide_color){0.5f, 0.5f, 0.5f, 1.0f})
#define TIDE_COLOR_CLEAR ((tide_color){0.0f, 0.0f, 0.0f, 0.0f})
