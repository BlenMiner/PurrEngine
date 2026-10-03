# A big game, made up here, that tidec has to compile within a generous time:
# a file of components and the input, and PARTS files each in a namespace of
# its own, with a singleton, an enum, structs with methods, constants,
# functions, an event and its handler, systems and views, so that every kind
# of name is looked up and written out thousands of times across many
# namespaces. With 150 parts (26,000 lines), compiling it ran out of memory
# after minutes, at 49 GB, while codegen built every declaration's C name
# again at each use and checked each local's name against every declaration
# (name_program in compiler/src/codegen.c); it takes seconds now, most of
# them the schedule's, as every part's systems write the same components. The
# ceiling is far above that, so machines of any speed pass, and a compiler
# that goes quadratic again fails.
#
# cmake -DTIDEC=<tidec> -DOUT=<folder> [-DPARTS=<n>] [-DSECONDS=<n>] -P scale.cmake

if(NOT PARTS)
    set(PARTS 150)
endif()
if(NOT SECONDS)
    set(SECONDS 300)
endif()

set(game "${OUT}/game")
file(REMOVE_RECURSE "${game}")
file(MAKE_DIRECTORY "${game}")

file(WRITE "${game}/a_main.tide" [[
// Made up by scale.cmake: the components every part shares, and the input.

component Body
{
    float2 position;
    float2 velocity;
    float radius = 10;
}

component Ball
{
    Color color = Color.white;
}

component Lifetime
{
    int ticks = 300;
}

component Aim
{
    float2 direction = float2(1, 0);
}

component Health
{
    int value = 10;
}

input PlayerInput
{
    float2 move;
    bool fire;

    Sample()
    {
        var keys = Devices.keyboard;
        if (keys.d.pressed || keys.rightArrow.pressed) move.x += 1;
        if (keys.a.pressed || keys.leftArrow.pressed) move.x -= 1;
        if (keys.w.pressed || keys.upArrow.pressed) move.y += 1;
        if (keys.s.pressed || keys.downArrow.pressed) move.y -= 1;
        fire = keys.space.pressed;
    }
}

scene Main { }

system Move(Time time, mut Body body)
{
    body.position += body.velocity * time.dt;
}

system Expire(mut Lifetime life)
{
    life.ticks -= 1;
    if (life.ticks <= 0) this.Destroy();
}
]])

set(lines 0)
foreach(k RANGE 1 ${PARTS})
    set(text "// Made up by scale.cmake: part ${k}, in a namespace of its own.
namespace Game.Part${k};

singleton Arena
{
    float2 halfSize = float2(400, 225);
    int rounds;
    int hits;
    Phase phase;
}

enum Phase
{
    Idle,
    Playing,
    Over,
}

struct Range
{
    float lo;
    float hi = 1;

    float Width() { return hi - lo; }

    mut void Scale(float factor)
    {
        lo *= factor;
        hi *= factor;
    }
}

struct Stats
{
    float health = 100;
    Range damage;

    bool IsDead()
    {
        if (health <= 0) return true;
        return false;
    }

    float Hit() { return damage.hi; }

    mut void TakeHit()
    {
        health -= Hit();
        if (IsDead()) health = 0;
    }
}

const float SPEED = 260;
const float FIRE_SPEED = SPEED + 160;
const int LIMIT = 3;

event Hit
{
    Entity attacker;
    int damage = 1;
}

float Speed(Body body)
{
    return Math.Length(body.velocity);
}

float2 Clamp(float2 position, float2 limit)
{
    mut var clamped = position;
    if (clamped.x > limit.x) clamped.x = limit.x;
    if (clamped.x < -limit.x) clamped.x = -limit.x;
    if (clamped.y > limit.y) clamped.y = limit.y;
    if (clamped.y < -limit.y) clamped.y = -limit.y;
    return clamped;
}

bool Inside(float2 position, float2 limit)
{
    var clamped = Clamp(position, limit);
    return clamped.x == position.x && clamped.y == position.y;
}

Stats Fresh(float health)
{
    mut var stats = Stats { health = health };
    stats.damage.Scale(2);
    if (stats.damage.Width() > 1) stats.TakeHit();
    return stats;
}

event(Spawned) Setup(with Main, mut Arena arena)
{
    arena.phase = Phase.Playing;
    Spawn(Body { radius = 16 }, Aim, Owner { player = PlayerID(0) });
    Spawn(Body { position = float2(-250, 120), velocity = float2(160, -90) }, Ball { color = Color(0.35, 0.67, 1) });
    Spawn(Body { position = float2(200, -80), velocity = float2(-120, 140), radius = 14 }, Ball, Health);
}

system Steer(PlayerInput input, mut Body body, mut Aim aim)
{
    mut var move = input.move;
    if (Math.Length(move) > 1) move = Math.Normalize(move);
    body.velocity = move * SPEED;
    if (Math.LengthSq(move) > 0) aim.direction = Math.Normalize(move);
}

system Fire(PlayerInput input, Body body, Aim aim, Arena arena)
{
    if (!input.fire.down || arena.phase != Phase.Playing) return;
    var muzzle = body.position + aim.direction * (body.radius + 10);
    Spawn(Body { position = muzzle, velocity = aim.direction * FIRE_SPEED, radius = 6 }, Ball, Lifetime);
}

system Bounce(Arena arena, mut Body body)
{
    var limit = arena.halfSize - body.radius;
    var inside = Inside(body.position, arena.halfSize);
    body.position = Clamp(body.position, limit);
    if (!inside) body.velocity = -body.velocity;
    if (Speed(body) > FIRE_SPEED * 2) body.velocity = Math.Normalize(body.velocity) * SPEED;
}

system Collide(Body body, Health health, mut Arena arena)
{
    if (health.value <= 0) return;
    if (body.position.x > arena.halfSize.x - LIMIT) this.Send(Hit { damage = 2 });
    arena.rounds += 1;
}

event(Hit hit) TakeHit(mut Health health, mut Arena arena)
{
    health.value -= hit.damage;
    arena.hits += 1;
    if (arena.hits > LIMIT * 100) arena.phase = Phase.Over;
}

local singleton Look
{
    bool open;
    bool aim = true;
    float zoom = 1.1;
    Color player = Color(1, 0.77, 0.24);
}

view DrawBalls(Body body, Ball ball, Look look)
{
    Draw.Circle(body.position, body.radius * look.zoom, ball.color);
}

view DrawPlayers(Body body, Aim aim, with Owner, Look look)
{
    Draw.Circle(body.position, body.radius, look.player);
    if (look.aim) Draw.Line(body.position, body.position + aim.direction * (body.radius + 12), look.player);
}

view Options(mut Look look, Arena arena)
{
    if (Devices.keyboard.escape.down) look.open = true;
    GUILayout.Area(Anchor.UpperLeft)
    {
        if (GUILayout.Button(\"Options\")) look.open = true;
        GUILayout.Label(\"rounds: \" + arena.rounds);
    }
    if (!look.open) return;
    GUILayout.Area(Anchor.MiddleCenter)
    {
        GUILayout.Toggle(\"Show aim\", look.aim);
        GUILayout.Slider(\"Zoom\", look.zoom, 0.5, 2);
        if (GUILayout.Button(\"Back\")) look.open = false;
    }
}
")
    file(WRITE "${game}/part${k}.tide" "${text}")
    string(REGEX MATCHALL "\n" newlines "${text}")
    list(LENGTH newlines n)
    math(EXPR lines "${lines} + ${n}")
endforeach()

file(GLOB files "${game}/*.tide")
list(SORT files)
file(MAKE_DIRECTORY "${OUT}/out")
string(TIMESTAMP started "%s")
execute_process(
    COMMAND "${TIDEC}" ${files} -o "${OUT}/out" --name game
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    TIMEOUT ${SECONDS})
string(TIMESTAMP ended "%s")
math(EXPR took "${ended} - ${started}")
if(result MATCHES "timeout")
    message(FATAL_ERROR "tidec took more than ${SECONDS} seconds on a game of ${PARTS} parts (${lines} lines)")
endif()
if(NOT result EQUAL 0)
    message(FATAL_ERROR "tidec failed on the game of ${PARTS} parts:\n${stdout}${stderr}")
endif()
if(NOT stderr STREQUAL "")
    message(FATAL_ERROR "tidec warned about the game of ${PARTS} parts:\n${stderr}")
endif()
message(STATUS "tidec compiled ${PARTS} parts (${lines} lines) in ${took} seconds")
