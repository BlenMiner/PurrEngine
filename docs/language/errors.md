# Errors

Errors in Tide are values, never exceptions. A function that can fail says so, and the code that calls it decides what to do about it, right where it calls.

## Functions that fail

`fails` after a function's parameters says it can fail, and with what. `fail` ends it with an error, as `return` does with a value:

```csharp
enum ParseError
{
    Empty,
    NotANumber,
}

int ParseScore(string text) fails ParseError
{
    if (text == "") fail ParseError.Empty;
    mut var score = 0;
    for (var i = 0; i < text.Length; i++)
    {
        var digit = "0123456789".IndexOf(text.Substring(i, 1));
        if (digit < 0) fail ParseError.NotANumber;
        score = score * 10 + digit;
    }
    return score;
}
```

An error is any type. Usually it's an enum, which names each way the function can fail. When it needs to say more, it's a struct:

```csharp
struct Refused
{
    int code;
    string message;
}

void Connect(int port) fails Refused
{
    if (port == 0) fail Refused { code = 1, message = "no port" };
}
```

A function that returns nothing fails the same way, and succeeds when it reaches its end. Methods can fail too.

## Handling an error

What a failing call gives is its value or its error. Before its value can be used, the call is unwrapped, in one of four ways:

```csharp
// ?? falls back to another value when it fails.
var fallback = ParseScore(text) ?? 0;

// is runs code only when it succeeds, with its value...
if (ParseScore(text) is int score)
{
    total += score;
}

// ...or only when it fails, with its error.
if (ParseScore(text) is ParseError why) { ... }
if (ParseScore(text) is ParseError.Empty) { ... }

// ! carries on with the type's default (0 for an int) when it fails.
var parsed = ParseScore(text)!;

// try passes the error on to the caller, which fails with the same type.
int Doubled(string text) fails ParseError
{
    var score = try ParseScore(text);
    return score * 2;
}
```

- `??`'s right side only runs when the left fails, as in C#, so, like the right side of `&&`, it can't spawn or draw widgets. Chains work: `ParseScore(a) ?? ParseScore(b) ?? 0`.
- A name after `is` goes in an `if`'s or a loop's condition, alone or joined with `&&`. It's in scope where the test is true: `if (ParseScore(t) is int score && score > 10)`, or `while (Next() is int item) { ... }`. Without a name, `is` is a test anywhere: `var empty = ParseScore(t) is ParseError.Empty;`.
- `!` never crashes. A struct's default is its fields' defaults, as `Stats { }` makes them.
- `try` binds like a unary operator, so `try ParseScore(a) + 1` adds 1 to the value. It only works in a function or method that fails with the same error type. Systems, views and handlers have no caller to pass an error to, so they handle it where they call.

`var` holds what a call gives as it is, to unwrap later:

```csharp
var result = ParseScore(text);
if (result is ParseError why) { ... }
var score = result ?? 0;
```

Using a failing call's value without unwrapping it is an error that says how to unwrap it. Calling one as a statement and ignoring its error is a warning; `!` says you meant to:

```csharp
Connect(7777);   // Warning: nothing handles its error
Connect(7777)!;  // Carries on, whatever happened
```

## Values that may be missing

`T?` is a value or nothing, for lookups where nothing isn't an error. `null` is nothing, and a value converts to a `T?` by itself, as in C#:

```csharp
int? Find(List<int> items, int wanted)
{
    for (var i = 0; i < items.Count; i++)
    {
        if (items[i] == wanted) return i;
    }
    return null;
}

var index = Find(items, 3) ?? -1;
if (Find(items, 3) is int at) { ... }
if (Find(items, 3) == null) { ... }
```

It unwraps the same way, with `??`, `is` and `!`, but not `try`: there's no error to pass on. Functions can return a `T?` and take one, and locals can hold one: `int? best = null;`. Fields can't yet.

## What doesn't fail

Most of Tide never fails, so there's nothing to handle:

- Arithmetic wraps on overflow, and dividing by zero gives 0.
- Reading a list past its end gives its element type's zero, and writing there does nothing.
- Text clamps positions past its end.
- `Math` is forgiving with bad values: `Math.Clamp` always returns a value in range.

Failures that happen later, rather than during a call, are events: a match that ends or a server that goes away sends `Disconnected` (see [Multiplayer](./multiplayer.md)).
