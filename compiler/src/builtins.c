#include "builtins.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"

// Every built-in math function maps to a purr/math.h function named
// purr_<lowercase name>_<type suffix>, for example Math.Dot on float3 is
// purr_dot_f3. Formulas and semantics follow Unity.Mathematics. Draw functions
// map to purr/draw.h, and GUI and GUILayout to purr/gui.h.

static const type T_F = {TY_FLOAT, NULL};
static const type T_F2 = {TY_FLOAT2, NULL};
static const type T_F3 = {TY_FLOAT3, NULL};
static const type T_COLOR = {TY_COLOR, NULL};
static const type T_STR = {TY_STRING, NULL};
static const type T_Q = {TY_QUATERNION, NULL};
static const type T_F4X4 = {TY_FLOAT4X4, NULL};
static const type T_NONE = {TY_VOID, NULL};
static const type T_BOOL = {TY_BOOL, NULL};

// The Anchor enum in signatures. The table outlives programs, so it can't
// hold the enum's declaration: calls match it against the program's own.
static const type T_ANCHOR = {TY_ENUM, NULL};
static const decl *anchor_decl;

void builtins_use(const decl *anchor)
{
    anchor_decl = anchor;
}

// A signature's type as a program sees it.
static type real_type(const type t)
{
    if (t.kind == TY_ENUM && !t.decl) return (type){TY_ENUM, (decl *)anchor_decl};
    return t;
}

static const char *signature_type_name(const type t)
{
    return t.kind == TY_ENUM && !t.decl ? "Anchor" : type_name(t);
}

// ---------------------------------------------------------------------------
// Component-wise functions: take float or int scalars and vectors, mixed
// freely as long as the vectors have one size; scalars widen to that size.

static const struct {
    const char *name;
    int argc;
    bool ints; // Works on ints too; otherwise ints convert to float.
} componentwise[] = {
    {"Abs", 1, true}, {"Sign", 1, true}, {"Min", 2, true}, {"Max", 2, true}, {"Clamp", 3, true},
    {"Floor", 1, false}, {"Ceil", 1, false}, {"Round", 1, false}, {"Trunc", 1, false}, {"Frac", 1, false},
    {"Sqrt", 1, false}, {"Rsqrt", 1, false}, {"Saturate", 1, false}, {"Radians", 1, false}, {"Degrees", 1, false},
    {"Sin", 1, false}, {"Cos", 1, false}, {"Tan", 1, false}, {"Asin", 1, false}, {"Acos", 1, false},
    {"Atan", 1, false}, {"Atan2", 2, false}, {"Exp", 1, false}, {"Exp2", 1, false}, {"Log", 1, false},
    {"Log2", 1, false}, {"Log10", 1, false}, {"Pow", 2, false}, {"Step", 2, false},
    {"Lerp", 3, false}, {"Unlerp", 3, false}, {"SmoothStep", 3, false},
};

#define COMPONENTWISE_COUNT (sizeof componentwise / sizeof componentwise[0])

// ---------------------------------------------------------------------------
// Fixed signatures, tried in order; the first whose parameters accept the
// arguments (with implicit int -> float widening) wins.
//
// The table is built once and kept for the whole process, so its strings are
// malloc'd rather than taken from the arena, which the language server resets.

#define MAX_PARAMS 6

typedef struct signature {
    const char *owner;
    const char *name;
    type result;
    int argc;
    type params[MAX_PARAMS];
    const char *c_name;
    const char *param_names; // "center, radius, color", or NULL
    const char *doc;         // One line for editors, or NULL
    unsigned mut;            // A bit per parameter the call changes: the argument's variable itself
    int gui;                 // GUI_ID and GUI_CONTAINER
} signature;

#define MAX_SIGNATURES 192

static signature signatures[MAX_SIGNATURES];
static int signature_count;

static const char *permanent(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (!out) {
        fprintf(stderr, "purrc: out of memory\n");
        exit(1);
    }
    memcpy(out, s, n);
    return out;
}

// purr_<lowercase name>_<suffix>, from the arena.
static const char *c_function(const char *name, const type t)
{
    char buf[128];
    int n = snprintf(buf, sizeof buf, "purr_");
    for (const char *p = name; *p && n < (int)sizeof buf - 1; p++) buf[n++] = (char)tolower((unsigned char)*p);
    snprintf(buf + n, sizeof buf - (size_t)n, "_%s", type_suffix(t));
    char *out = arena_alloc(strlen(buf) + 1);
    memcpy(out, buf, strlen(buf));
    return out;
}

static signature *add(const char *owner, const char *name, const type result, const char *c_name,
                      const int argc, const type p0, const type p1, const type p2)
{
    if (signature_count == MAX_SIGNATURES) {
        fprintf(stderr, "purrc: too many built-in signatures (raise MAX_SIGNATURES)\n");
        exit(1);
    }
    signature *s = &signatures[signature_count++];
    *s = (signature){owner, name, result, argc, {p0, p1, p2, T_NONE, T_NONE, T_NONE}, permanent(c_name), NULL, NULL, 0, 0};
    return s;
}

// A GUI function from its parameters as PurrLang writes them:
// "string label, mut float value, float min, float max".
static void add_gui(const char *owner, const char *name, const type result, const char *c_name, const char *params,
                    const int gui, const char *doc)
{
    signature *s = add(owner, name, result, c_name, 0, T_NONE, T_NONE, T_NONE);
    s->gui = gui;
    s->doc = doc;
    sb names = {0};
    for (const char *p = params; *p;) {
        const char *end = strchr(p, ',');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        char buf[64];
        snprintf(buf, sizeof buf, "%.*s", (int)n, p);
        const bool mut = strncmp(buf, "mut ", 4) == 0;
        char *type_text = buf + (mut ? 4 : 0);
        char *space = strchr(type_text, ' ');
        *space = '\0';
        type t = {TY_ERROR, NULL};
        if (strcmp(type_text, "string") == 0) t = T_STR;
        else if (strcmp(type_text, "Anchor") == 0) t = T_ANCHOR;
        else builtin_type_named(str_from(type_text), &t);
        if (mut) s->mut |= 1u << s->argc;
        s->params[s->argc++] = t;
        sb_printf(&names, "%s%s", names.len ? ", " : "", space + 1);
        p = end ? end + 2 : p + n;
    }
    s->param_names = permanent(names.data ? names.data : "");
}

// A widget in both GUI, at a rect, and GUILayout, laid out: purr_gui_<c> and
// purr_gui_layout_<c>.
static void add_widget(const char *name, const type result, const char *c, const char *params, const int gui,
                       const char *doc)
{
    char c_name[64];
    char with_rect[256];
    snprintf(with_rect, sizeof with_rect, params[0] ? "Rect rect, %s" : "Rect rect", params);
    snprintf(c_name, sizeof c_name, "purr_gui_%s", c);
    add_gui("GUI", name, result, c_name, with_rect, gui, doc);
    snprintf(c_name, sizeof c_name, "purr_gui_layout_%s", c);
    add_gui("GUILayout", name, result, c_name, params, gui, doc);
}

static void describe(signature *s, const char *param_names, const char *doc)
{
    s->param_names = param_names;
    s->doc = doc;
}

static void build_signatures(void)
{
    if (signature_count > 0) return;

    for (int n = 2; n <= 4; n++) {
        const type v = vector_type(true, n);
        add("Math", "Dot", T_F, c_function("dot", v), 2, v, v, T_NONE);
        add("Math", "Length", T_F, c_function("length", v), 1, v, T_NONE, T_NONE);
        add("Math", "LengthSq", T_F, c_function("lengthsq", v), 1, v, T_NONE, T_NONE);
        add("Math", "Distance", T_F, c_function("distance", v), 2, v, v, T_NONE);
        add("Math", "DistanceSq", T_F, c_function("distancesq", v), 2, v, v, T_NONE);
        add("Math", "Normalize", v, c_function("normalize", v), 1, v, T_NONE, T_NONE);
        add("Math", "NormalizeSafe", v, c_function("normalizesafe", v), 1, v, T_NONE, T_NONE);
        add("Math", "Reflect", v, c_function("reflect", v), 2, v, v, T_NONE);
        add("Math", "Csum", T_F, c_function("csum", v), 1, v, T_NONE, T_NONE);
        add("Math", "Cmin", T_F, c_function("cmin", v), 1, v, T_NONE, T_NONE);
        add("Math", "Cmax", T_F, c_function("cmax", v), 1, v, T_NONE, T_NONE);
    }
    add("Math", "Cross", T_F3, "purr_cross_f3", 2, T_F3, T_F3, T_NONE);

    // Quaternions
    add("Math", "Dot", T_F, "purr_dot_q", 2, T_Q, T_Q, T_NONE);
    add("Math", "Normalize", T_Q, "purr_normalize_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "NormalizeSafe", T_Q, "purr_normalizesafe_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Mul", T_Q, "purr_mul_q", 2, T_Q, T_Q, T_NONE);
    add("Math", "Mul", T_F3, "purr_rotate_q", 2, T_Q, T_F3, T_NONE);
    add("Math", "Rotate", T_F3, "purr_rotate_q", 2, T_Q, T_F3, T_NONE);
    add("Math", "Inverse", T_Q, "purr_inverse_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Conjugate", T_Q, "purr_conjugate_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Slerp", T_Q, "purr_slerp_q", 3, T_Q, T_Q, T_F);
    add("Math", "Nlerp", T_Q, "purr_nlerp_q", 3, T_Q, T_Q, T_F);
    add("Math", "Forward", T_F3, "purr_forward_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Up", T_F3, "purr_up_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Right", T_F3, "purr_right_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Angle", T_F, "purr_angle_q", 2, T_Q, T_Q, T_NONE);

    // Matrices
    for (int n = 2; n <= 4; n++) {
        const type m = matrix_type(n);
        const type v = vector_type(true, n);
        char mul_vec[64];
        snprintf(mul_vec, sizeof mul_vec, "purr_mul_%s_%s", type_suffix(m), type_suffix(v));
        add("Math", "Mul", m, c_function("mul", m), 2, m, m, T_NONE);
        add("Math", "Mul", v, mul_vec, 2, m, v, T_NONE);
        add("Math", "Transpose", m, c_function("transpose", m), 1, m, T_NONE, T_NONE);
        add("Math", "Inverse", m, c_function("inverse", m), 1, m, T_NONE, T_NONE);
        add("Math", "Determinant", T_F, c_function("determinant", m), 1, m, T_NONE, T_NONE);
    }
    add("Math", "Transform", T_F3, "purr_transform_f4x4", 2, T_F4X4, T_F3, T_NONE);
    add("Math", "Rotate", T_F3, "purr_rotate_f4x4", 2, T_F4X4, T_F3, T_NONE);

    // Ways to build quaternions and matrices
    describe(add("quaternion", "AxisAngle", T_Q, "purr_axisangle_q", 2, T_F3, T_F, T_NONE),
             "axis, angle", "A rotation of `angle` radians around `axis`.");
    describe(add("quaternion", "Euler", T_Q, "purr_euler_q", 1, T_F3, T_NONE, T_NONE),
             "radians", "A rotation from Euler angles in radians: Z first, then X, then Y.");
    describe(add("quaternion", "LookRotation", T_Q, "purr_lookrotation_q", 2, T_F3, T_F3, T_NONE),
             "forward, up", "A rotation that looks along `forward`, with `up` as up.");
    describe(add("float4x4", "TRS", T_F4X4, "purr_trs_f4x4", 3, T_F3, T_Q, T_F3),
             "translation, rotation, scale", "A transform: scale, then rotate, then translate.");
    describe(add("float4x4", "Translate", T_F4X4, "purr_translate_f4x4", 1, T_F3, T_NONE, T_NONE),
             "translation", "A translation matrix.");

    // Drawing, in views: purr/draw.h. Codegen passes the view's draw list first.
    describe(add("Draw", "Clear", T_NONE, "purr_draw_clear", 1, T_COLOR, T_NONE, T_NONE),
             "color", "Fills the whole screen.");
    describe(add("Draw", "Camera", T_NONE, "purr_draw_camera", 2, T_F2, T_F, T_NONE),
             "center, size",
             "Sets the camera for the Draw calls after it. `center` is the world position at the middle of the "
             "screen and `size` is half the visible height, like Unity's orthographic size.");
    describe(add("Draw", "Circle", T_NONE, "purr_draw_circle", 3, T_F2, T_F, T_COLOR),
             "center, radius, color", "A filled circle.");
    describe(add("Draw", "WireCircle", T_NONE, "purr_draw_wire_circle", 3, T_F2, T_F, T_COLOR),
             "center, radius, color", "A circle outline.");
    describe(add("Draw", "Rect", T_NONE, "purr_draw_rect", 3, T_F2, T_F2, T_COLOR),
             "center, size, color", "A filled rectangle.");
    describe(add("Draw", "WireRect", T_NONE, "purr_draw_wire_rect", 3, T_F2, T_F2, T_COLOR),
             "center, size, color", "A rectangle outline.");
    describe(add("Draw", "Line", T_NONE, "purr_draw_line", 3, T_F2, T_F2, T_COLOR),
             "from, to, color", "A line.");
    signature *text = add("Draw", "Text", T_NONE, "purr_draw_text", 3, T_STR, T_F2, T_F);
    text->argc = 4;
    text->params[3] = T_COLOR;
    describe(text, "text, position, size, color", "Text: `position` is its top left corner and `size` its height.");

    // Text's methods: purr/text.h, with the text first. Positions and lengths
    // count characters, and are clamped to the text, never out of range.
    const type t_int = {TY_INT, NULL};
    describe(add("string", "Contains", T_BOOL, "purr_str_contains", 1, T_STR, T_NONE, T_NONE),
             "value", "Whether `value` is in the text.");
    describe(add("string", "StartsWith", T_BOOL, "purr_str_starts_with", 1, T_STR, T_NONE, T_NONE),
             "value", "Whether the text starts with `value`.");
    describe(add("string", "EndsWith", T_BOOL, "purr_str_ends_with", 1, T_STR, T_NONE, T_NONE),
             "value", "Whether the text ends with `value`.");
    describe(add("string", "IndexOf", t_int, "purr_str_index_of", 1, T_STR, T_NONE, T_NONE),
             "value", "Where `value` first is in the text, counting characters from 0, or -1.");
    describe(add("string", "Substring", T_STR, "purr_str_substring_from", 1, t_int, T_NONE, T_NONE),
             "start", "The text from character `start` on.");
    describe(add("string", "Substring", T_STR, "purr_str_substring", 2, t_int, t_int, T_NONE),
             "start, length", "`length` characters of the text, from character `start`.");
    describe(add("string", "ToUpper", T_STR, "purr_str_to_upper", 0, T_NONE, T_NONE, T_NONE),
             NULL, "The text in upper case: ASCII letters only, for now.");
    describe(add("string", "ToLower", T_STR, "purr_str_to_lower", 0, T_NONE, T_NONE, T_NONE),
             NULL, "The text in lower case: ASCII letters only, for now.");
    describe(add("string", "Trim", T_STR, "purr_str_trim", 0, T_NONE, T_NONE, T_NONE),
             NULL, "The text without spaces, tabs and new lines at its start and end.");
    describe(add("string", "Replace", T_STR, "purr_str_replace", 2, T_STR, T_STR, T_NONE),
             "from, to", "The text with every `from` in it replaced by `to`.");

    // The GUI, in views: purr/gui.h. Codegen passes the view's GUI first, then
    // the widget's ID, then a mut argument's address.
    add_widget("Label", T_NONE, "label", "string text", 0, "Text.");
    add_widget("Button", T_BOOL, "button", "string text", GUI_ID, "A button. Returns whether it was pressed.");
    add_widget("Toggle", T_BOOL, "toggle", "string text, mut bool value", GUI_ID,
               "A checkbox for `value`. Returns whether it changed it.");
    add_widget("Slider", T_BOOL, "slider", "string label, mut float value, float min, float max", GUI_ID,
               "A slider for `value`, from `min` to `max`. Returns whether it changed it.");
    add_widget("IntSlider", T_BOOL, "int_slider", "string label, mut int value, int min, int max", GUI_ID,
               "A slider for a whole number, from `min` to `max`. Returns whether it changed it.");
    add_widget("IntField", T_BOOL, "int_field", "string label, mut int value", GUI_ID,
               "A field to type a whole number into. Returns whether it changed `value`.");
    add_widget("FloatField", T_BOOL, "float_field", "string label, mut float value", GUI_ID,
               "A field to type a number into. Returns whether it changed `value`.");
    add_widget("Float2Field", T_BOOL, "float2_field", "string label, mut float2 value", GUI_ID,
               "Fields for a float2's x and y. Returns whether they changed `value`.");
    add_widget("Float3Field", T_BOOL, "float3_field", "string label, mut float3 value", GUI_ID,
               "Fields for a float3's x, y and z. Returns whether they changed `value`.");
    add_widget("Float4Field", T_BOOL, "float4_field", "string label, mut float4 value", GUI_ID,
               "Fields for a float4's x, y, z and w. Returns whether they changed `value`.");
    add_widget("ColorField", T_BOOL, "color_field", "string label, mut Color value", GUI_ID,
               "A color's swatch, and fields for its r, g, b and a. Returns whether they changed `value`.");
    add_widget("TextField", T_BOOL, "text_field", "string label, mut string value", GUI_ID,
               "A field to type text into. It changes `value` as the player types, and returns whether it did.");
    add_gui("GUILayout", "Space", T_NONE, "purr_gui_layout_space", "float size", 0,
            "Empty space: down in a vertical container, across in a horizontal one.");
    add_gui("GUILayout", "Vertical", T_NONE, "purr_gui_begin_vertical", "", GUI_ID | GUI_CONTAINER,
            "Stacks the widgets in its block top to bottom.");
    add_gui("GUILayout", "Horizontal", T_NONE, "purr_gui_begin_horizontal", "", GUI_ID | GUI_CONTAINER,
            "Puts the widgets in its block side by side.");
    add_gui("GUILayout", "Area", T_NONE, "purr_gui_begin_area_at", "Anchor anchor", GUI_ID | GUI_CONTAINER,
            "A panel on the screen, sized to its block's widgets, at one of nine anchors like `Anchor.MiddleCenter`.");
    add_gui("GUILayout", "Area", T_NONE, "purr_gui_begin_area", "Rect rect", GUI_ID | GUI_CONTAINER,
            "A panel on the screen at `rect`, with its block's widgets laid out inside.");
    add_gui("GUILayout", "Modal", T_NONE, "purr_gui_begin_modal", "Anchor anchor, mut bool open",
            GUI_ID | GUI_CONTAINER | GUI_SKIPS,
            "A panel over the whole screen while `open` is true, like a pause menu. While it's up, the widgets "
            "outside it don't work, the game and views get no input, and back (Escape or the east button) closes "
            "it.");
}

// ---------------------------------------------------------------------------
// Static members: constants reached through a type or Math.

static const struct {
    const char *owner;
    const char *member;
    type_kind kind;
    const char *c_constant;
} members[] = {
    {"Math", "PI", TY_FLOAT, "PURR_PI_F"},
    {"Math", "TAU", TY_FLOAT, "PURR_TAU_F"},
    {"Math", "E", TY_FLOAT, "PURR_E_F"},
    {"quaternion", "identity", TY_QUATERNION, "purr_identity_q()"},
    {"float2x2", "identity", TY_FLOAT2X2, "purr_identity_f2x2()"},
    {"float3x3", "identity", TY_FLOAT3X3, "purr_identity_f3x3()"},
    {"float4x4", "identity", TY_FLOAT4X4, "purr_identity_f4x4()"},
    {"Color", "white", TY_COLOR, "PURR_COLOR_WHITE"},
    {"Color", "black", TY_COLOR, "PURR_COLOR_BLACK"},
    {"Color", "red", TY_COLOR, "PURR_COLOR_RED"},
    {"Color", "green", TY_COLOR, "PURR_COLOR_GREEN"},
    {"Color", "blue", TY_COLOR, "PURR_COLOR_BLUE"},
    {"Color", "yellow", TY_COLOR, "PURR_COLOR_YELLOW"},
    {"Color", "cyan", TY_COLOR, "PURR_COLOR_CYAN"},
    {"Color", "magenta", TY_COLOR, "PURR_COLOR_MAGENTA"},
    {"Color", "gray", TY_COLOR, "PURR_COLOR_GRAY"},
    {"Color", "clear", TY_COLOR, "PURR_COLOR_CLEAR"},
    {"Screen", "width", TY_FLOAT, "purr_ui->width"},
    {"Screen", "height", TY_FLOAT, "purr_ui->height"},
    {"Screen", "scale", TY_FLOAT, "purr_ui->scale"},
};

#define MEMBER_COUNT (sizeof members / sizeof members[0])

// ---------------------------------------------------------------------------

bool builtin_owner(const str name)
{
    type ignored;
    return str_eq_c(name, "Math") || str_eq_c(name, "Draw") || str_eq_c(name, "GUI") || str_eq_c(name, "GUILayout")
        || str_eq_c(name, "Screen") || builtin_type_named(name, &ignored);
}

// Widest type of a component-wise call's arguments, or false if they don't fit together.
static bool unify(const expr *e, const bool ints_ok, type *out)
{
    int dim = 1;
    bool is_float = !ints_ok;
    for (int i = 0; i < e->args.count; i++) {
        const type t = e->args.items[i]->type;
        if (!type_is_numeric(t)) return false;
        const int d = type_dim(t);
        if (d > 1) {
            if (dim > 1 && d != dim) return false;
            dim = d;
        }
        if (type_is_float_based(t)) is_float = true;
    }
    *out = vector_type(is_float, dim);
    return true;
}

static void arg_list(const expr *e, char *buf, const size_t size)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < e->args.count && len + 1 < size; i++) {
        const int n = snprintf(buf + len, size - len, "%s%s", i ? ", " : "", type_name(e->args.items[i]->type));
        if (n > 0) len += (size_t)n;
    }
}

// "Draw.Circle(float2 center, float radius, Color color)", plus " -> float" for results.
static void format_signature(const signature *s, sb *out)
{
    sb_printf(out, "%s.%s(", s->owner, s->name);
    const char *names = s->param_names;
    for (int a = 0; a < s->argc; a++) {
        sb_printf(out, "%s%s%s", a ? ", " : "", s->mut & (1u << a) ? "mut " : "", signature_type_name(s->params[a]));
        if (names) {
            const char *end = strchr(names, ',');
            const size_t n = end ? (size_t)(end - names) : strlen(names);
            sb_put(out, " ");
            sb_putn(out, names, n);
            names = end ? end + 2 : NULL;
        }
    }
    if (s->gui & GUI_CONTAINER) sb_printf(out, "%sBlock content", s->argc ? ", " : "");
    sb_put(out, ")");
    if (s->result.kind != TY_VOID) sb_printf(out, " -> %s", type_name(s->result));
}

type resolve_builtin_call(const str owner, expr *e)
{
    for (int i = 0; i < e->args.count; i++) {
        if (e->args.items[i]->type.kind == TY_ERROR) return (type){TY_ERROR, NULL};
    }
    e->call = CALL_BUILTIN;
    e->arg_want.count = 0;

    if (str_eq_c(owner, "Math")) {
        for (size_t i = 0; i < COMPONENTWISE_COUNT; i++) {
            if (!str_eq_c(e->name, componentwise[i].name)) continue;
            if (e->args.count != componentwise[i].argc) {
                diag_error(e->at, "Math.%s takes %d argument%s, not %d", componentwise[i].name, componentwise[i].argc,
                           componentwise[i].argc == 1 ? "" : "s", e->args.count);
                return (type){TY_ERROR, NULL};
            }
            type target;
            if (!unify(e, componentwise[i].ints, &target)) {
                char args[256];
                arg_list(e, args, sizeof args);
                diag_error(e->at, "Math.%s can't take (%s)", componentwise[i].name, args);
                diag_note("it takes numbers and vectors; vectors in one call must be the same size");
                return (type){TY_ERROR, NULL};
            }
            for (int a = 0; a < e->args.count; a++) vec_push(e->arg_want, target);
            e->c_callee = c_function(componentwise[i].name, target);
            return target;
        }
    }

    build_signatures();
    bool known = false;
    for (int i = 0; i < signature_count; i++) {
        const signature *s = &signatures[i];
        if (!str_eq_c(owner, s->owner) || !str_eq_c(e->name, s->name)) continue;
        known = true;
        if (s->argc != e->args.count) continue;
        bool fits = true;
        for (int a = 0; a < s->argc; a++) {
            const type want = real_type(s->params[a]);
            const type have = e->args.items[a]->type;
            // A mut argument is the variable itself, so its type matches exactly.
            if (s->mut & (1u << a) ? want.kind != have.kind || want.decl != have.decl : !type_assignable(want, have)) {
                fits = false;
            }
        }
        if (!fits) continue;
        for (int a = 0; a < s->argc; a++) vec_push(e->arg_want, real_type(s->params[a]));
        e->c_callee = s->c_name;
        e->arg_mut = s->mut;
        e->gui = s->gui;
        return s->result;
    }

    if (!known) {
        diag_error(e->at, STR_FMT " has no function '" STR_FMT "'", STR_ARG(owner), STR_ARG(e->name));
        suggestion s = suggest_start(e->name);
        for (size_t i = 0; i < COMPONENTWISE_COUNT && str_eq_c(owner, "Math"); i++) {
            suggest_consider_c(&s, componentwise[i].name);
        }
        for (int i = 0; i < signature_count; i++) {
            if (str_eq_c(owner, signatures[i].owner)) suggest_consider_c(&s, signatures[i].name);
        }
        suggest_note(&s);
        return (type){TY_ERROR, NULL};
    }
    char args[256];
    arg_list(e, args, sizeof args);
    diag_error(e->at, "no version of " STR_FMT "." STR_FMT " takes (%s)", STR_ARG(owner), STR_ARG(e->name), args);
    bool changes = false;
    for (int i = 0; i < signature_count; i++) {
        const signature *s = &signatures[i];
        if (!str_eq_c(owner, s->owner) || !str_eq_c(e->name, s->name)) continue;
        sb line = {0};
        format_signature(s, &line);
        diag_note("%s", line.data);
        changes |= s->mut != 0;
    }
    if (changes) diag_note("a mut argument is the variable it changes, so its type is exactly the parameter's");
    return (type){TY_ERROR, NULL};
}

type resolve_builtin_member(const str owner, expr *e)
{
    for (size_t i = 0; i < MEMBER_COUNT; i++) {
        if (str_eq_c(owner, members[i].owner) && str_eq_c(e->member, members[i].member)) {
            e->c_constant = members[i].c_constant;
            return (type){members[i].kind, NULL};
        }
    }
    diag_error(e->at, STR_FMT " has no member '" STR_FMT "'", STR_ARG(owner), STR_ARG(e->member));
    suggestion s = suggest_start(e->member);
    for (size_t i = 0; i < MEMBER_COUNT; i++) {
        if (str_eq_c(owner, members[i].owner)) suggest_consider_c(&s, members[i].member);
    }
    suggest_note(&s);
    return (type){TY_ERROR, NULL};
}

const char *builtin_function_owner(const str name)
{
    build_signatures();
    for (size_t i = 0; i < COMPONENTWISE_COUNT; i++) {
        if (str_eq_c(name, componentwise[i].name)) return "Math";
    }
    for (int i = 0; i < signature_count; i++) {
        if (str_eq_c(name, signatures[i].name)) return signatures[i].owner;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// For editors

static const char *componentwise_doc(const char *name)
{
    static const struct {
        const char *name;
        const char *doc;
    } docs[] = {
        {"Clamp", "Clamps `x` between `a` and `b`, component by component. NaN gives `a`."},
        {"Min", "The smaller of `a` and `b`, component by component. If one is NaN, the other."},
        {"Max", "The larger of `a` and `b`, component by component. If one is NaN, the other."},
        {"Lerp", "Linear interpolation: `a + (b - a) * t`, component by component."},
        {"Unlerp", "The `t` for which Lerp(a, b, t) gives `x`."},
        {"SmoothStep", "Smooth Hermite interpolation between 0 and 1 as `x` goes from `a` to `b`."},
        {"Step", "1 where `x >= edge`, 0 elsewhere."},
        {"Saturate", "Clamps between 0 and 1. NaN gives 0."},
        {"Round", "Rounds to the nearest integer; ties go to even."},
        {"Frac", "The fractional part: `x - Floor(x)`."},
        {"Rsqrt", "1 / Sqrt(x)."},
        {"Radians", "Degrees to radians."},
        {"Degrees", "Radians to degrees."},
        {"Atan2", "The angle of the point (x, y) in radians. Takes (y, x), as in Unity."},
    };
    for (size_t i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        if (strcmp(docs[i].name, name) == 0) return docs[i].doc;
    }
    return "Component by component, on numbers and vectors.";
}

static const char *componentwise_params(const int argc, const char *name)
{
    if (strcmp(name, "Clamp") == 0) return "x, a, b";
    if (strcmp(name, "Lerp") == 0) return "a, b, t";
    if (strcmp(name, "Unlerp") == 0 || strcmp(name, "SmoothStep") == 0) return "a, b, x";
    if (strcmp(name, "Step") == 0) return "edge, x";
    if (strcmp(name, "Atan2") == 0) return "y, x";
    if (strcmp(name, "Pow") == 0) return "x, y";
    return argc == 1 ? "x" : argc == 2 ? "a, b" : "a, b, c";
}

// "Circle(${1:center}, ${2:radius}, ${3:color})" from "center, radius, color".
static void format_snippet(const char *name, const char *param_names, sb *out)
{
    sb_printf(out, "%s(", name);
    int index = 1;
    for (const char *p = param_names; p && *p;) {
        const char *end = strchr(p, ',');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        sb_printf(out, "%s${%d:", index > 1 ? ", " : "", index);
        sb_putn(out, p, n);
        sb_put(out, "}");
        index++;
        p = end ? end + 2 : NULL;
    }
    sb_put(out, ")");
}

void builtin_list_members(const str owner, void (*visit)(void *user, const builtin_member *m), void *user)
{
    build_signatures();

    if (str_eq_c(owner, "Math")) {
        for (size_t i = 0; i < COMPONENTWISE_COUNT; i++) {
            const char *params = componentwise_params(componentwise[i].argc, componentwise[i].name);
            sb detail = {0};
            sb_printf(&detail, "Math.%s(%s)", componentwise[i].name, params);
            sb snippet = {0};
            format_snippet(componentwise[i].name, params, &snippet);
            const builtin_member m = {componentwise[i].name, true, detail.data, componentwise_doc(componentwise[i].name),
                                      snippet.data};
            visit(user, &m);
        }
    }

    for (int i = 0; i < signature_count; i++) {
        const signature *s = &signatures[i];
        if (!str_eq_c(owner, s->owner)) continue;
        bool seen = false; // Overloads appear once, under their first signature.
        for (int j = 0; j < i; j++) {
            if (strcmp(signatures[j].owner, s->owner) == 0 && strcmp(signatures[j].name, s->name) == 0) seen = true;
        }
        if (seen) continue;
        sb detail = {0};
        format_signature(s, &detail);
        sb snippet = {0};
        if (s->param_names) format_snippet(s->name, s->param_names, &snippet);
        else sb_printf(&snippet, "%s($1)", s->name);
        if (s->gui & GUI_CONTAINER) sb_put(&snippet, "\n{\n\t$0\n}");
        const builtin_member m = {s->name, true, detail.data, s->doc, snippet.data};
        visit(user, &m);
    }

    for (size_t i = 0; i < MEMBER_COUNT; i++) {
        if (!str_eq_c(owner, members[i].owner)) continue;
        sb detail = {0};
        sb_printf(&detail, "%s.%s: %s", members[i].owner, members[i].member, type_name((type){members[i].kind, NULL}));
        const builtin_member m = {members[i].member, false, detail.data, NULL, NULL};
        visit(user, &m);
    }
}

void builtin_signatures(const str owner, const str name, void (*visit)(void *user, const char *label, const char *doc),
                        void *user)
{
    build_signatures();
    for (size_t i = 0; i < COMPONENTWISE_COUNT && str_eq_c(owner, "Math"); i++) {
        if (!str_eq_c(name, componentwise[i].name)) continue;
        sb label = {0};
        sb_printf(&label, "Math.%s(%s)", componentwise[i].name,
                  componentwise_params(componentwise[i].argc, componentwise[i].name));
        visit(user, label.data, componentwise_doc(componentwise[i].name));
        return;
    }
    for (int i = 0; i < signature_count; i++) {
        const signature *s = &signatures[i];
        if (!str_eq_c(owner, s->owner) || !str_eq_c(name, s->name)) continue;
        sb label = {0};
        format_signature(s, &label);
        visit(user, label.data, s->doc);
    }
}

bool builtin_describe(const str owner, const str name, sb *out)
{
    build_signatures();
    bool found = false;

    for (size_t i = 0; i < COMPONENTWISE_COUNT && str_eq_c(owner, "Math"); i++) {
        if (!str_eq_c(name, componentwise[i].name)) continue;
        sb_printf(out, "Math.%s(%s)\n", componentwise[i].name,
                  componentwise_params(componentwise[i].argc, componentwise[i].name));
        sb_printf(out, "\n%s", componentwise_doc(componentwise[i].name));
        return true;
    }

    const char *doc = NULL;
    for (int i = 0; i < signature_count; i++) {
        const signature *s = &signatures[i];
        if (!str_eq_c(owner, s->owner) || !str_eq_c(name, s->name)) continue;
        format_signature(s, out);
        sb_put(out, "\n");
        if (s->doc) doc = s->doc;
        found = true;
    }
    if (found) {
        if (doc) sb_printf(out, "\n%s", doc);
        return true;
    }

    for (size_t i = 0; i < MEMBER_COUNT; i++) {
        if (str_eq_c(owner, members[i].owner) && str_eq_c(name, members[i].member)) {
            sb_printf(out, "%s.%s: %s", members[i].owner, members[i].member, type_name((type){members[i].kind, NULL}));
            return true;
        }
    }
    return false;
}
