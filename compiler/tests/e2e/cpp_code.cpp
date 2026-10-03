// The C++ side of cpp.tide: C++ with no C++ runtime, as tide builds a game's
// .cpp files. Nothing of the standard library is included (it isn't there),
// nothing throws, and there's no RTTI. What the game calls is extern "C".
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// What a C++ runtime would define, and code that wants it defines itself:
// new and delete over malloc and free, placement new, and what a pure
// virtual function's slot holds.
void *operator new(const size_t size) { return malloc(size); }
void *operator new[](const size_t size) { return malloc(size); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
inline void *operator new(size_t, void *place) noexcept { return place; }
extern "C" void __cxa_pure_virtual() { abort(); }

// A global with a constructor, which runs before the game does, and a
// destructor, which runs as the program ends. malloc keeps the compiler from
// working the value out itself: the constructor has to run.
struct Startup {
    int *value;
    Startup() : value(static_cast<int *>(malloc(sizeof(int)))) { *value = 10; }
    ~Startup() { free(value); }
};
static Startup startup;

struct Shape {
    virtual float Area() const = 0;
};

struct Square : Shape {
    float side;
    explicit Square(const float side) : side(side) {}
    float Area() const override { return side * side; }
};

template <typename T, typename Less> static T Largest(const T a, const T b, const Less less) { return less(a, b) ? b : a; }

// The game's struct, with the same fields, and a method.
struct Totals {
    int32_t count;
    float sum;
    void Add(const float value)
    {
        count++;
        sum += value;
    }
};

static int live;

// A virtual destructor: deleting through it uses operator delete.
struct Body {
    float mass;
    explicit Body(const float mass) : mass(mass) { live++; }
    virtual ~Body() { live--; }
    virtual float Weight() const { return mass * 10.0f; }
};

// A function's static with a constructor, which runs the first time through.
struct Calls {
    int count;
    Calls() : count(*startup.value) {}
};

extern "C" {

int cpp_started(void) { return *startup.value; }

float cpp_area(const float side)
{
    const Square square(side);
    const Shape &shape = square;
    return shape.Area();
}

int cpp_largest(const int a, const int b)
{
    return Largest(a, b, [](const int x, const int y) { return x < y; });
}

void cpp_add(Totals *totals, const float value) { totals->Add(value); }

// Two bodies, one in memory that's there already and one from new: their
// weights, then how many there were, then how many are left.
int cpp_made(void)
{
    alignas(Body) unsigned char room[sizeof(Body)];
    Body *placed = new (room) Body(2.0f);
    Body *made = new Body(3.0f);
    const int weight = static_cast<int>(placed->Weight() + made->Weight());
    const int made_count = live;
    delete made;
    placed->~Body();
    return weight * 100 + made_count * 10 + live;
}

int cpp_calls(void)
{
    static Calls calls;
    return ++calls.count;
}

} // extern "C"
