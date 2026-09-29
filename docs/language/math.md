# Math

PurrLang's math follows Unity.Mathematics, with PurrLang's naming: functions live on `Math`, in PascalCase. It's all deterministic: the same inputs give the same bits on every platform (see [Determinism](../engine/determinism.md)).

```csharp
quaternion spin = quaternion.AxisAngle(float3(0, 1, 0), input.turn * time.dt);
trs.rotation = Math.Mul(trs.rotation, spin);
float3 forward = Math.Rotate(trs.rotation, float3(0, 0, 1));
trs.position += Math.Normalize(forward) * 5 * time.dt;
float2 flat = trs.position.xz;
```

## Types

| Type | What it is |
|---|---|
| `float2`, `float3`, `float4` | Float vectors |
| `int2`, `int3`, `int4` | Int vectors |
| `quaternion` | A rotation |
| `float2x2`, `float3x3`, `float4x4` | Matrices, stored column by column |

They're lowercase built-in value types, like `float`. Angles are in radians: `Math.Radians(degrees)` and `Math.Degrees(radians)` convert.

## Building values

- Vectors take any mix of scalars and vectors that adds up to their size, `float4(v.xy, 0, 1)`, or one scalar for every component, `float3(1)`.
- `float3(i)` and `int3(f)` convert between int and float vectors, and `int(x)` and `float(x)` between scalars. Float to int truncates toward zero, saturates at the int range, and turns NaN into 0.
- An int vector converts to a float vector of the same size by itself, like `int` to `float`.
- `quaternion(x, y, z, w)`, `quaternion(float4)` and `quaternion(float3x3)`.
- Matrices take one vector per column, `float3x3(c0, c1, c2)`, or all their numbers row by row: in `float2x2(1, 2, 3, 4)`, `1, 2` is the first row. Also `float3x3(quaternion)`, and `float4x4(rotation, translation)` from a `float3x3` and a `float3`.

## Members

- Vectors have `x`, `y`, `z` and `w`.
- Swizzles read any combination: `v.xz`, `v.zyx`, `v.xxyy`. They can be assigned too, as long as no component repeats: `v.xz = float2(1, 2)`.
- A `quaternion`'s `value` is a `float4`, with (x, y, z) as the vector part.
- A matrix's columns are `c0` to `c3`.

## Constants

`Math.PI`, `Math.TAU`, `Math.E`, `quaternion.identity`, `float2x2.identity`, `float3x3.identity` and `float4x4.identity`.

And these build values:

- `quaternion.AxisAngle(axis, angle)`
- `quaternion.Euler(radians)`: Z first, then X, then Y, Unity's default order
- `quaternion.LookRotation(forward, up)`
- `float4x4.TRS(translation, rotation, scale)`
- `float4x4.Translate(translation)`

## Operators

- `+ - * /` work component by component on vectors, and `%` on int vectors. A scalar widens to the vector's size, and int to float, so `v * 2 + 1` works.
- Matrices have `+` and `-` with their own type, and `*` and `/` by a number. There's no `*` between matrices, or between a matrix and a vector: that's `Math.Mul`.
- Quaternions have no operators: combine rotations with `Math.Mul`, and rotate vectors with `Math.Rotate`.
- Comparisons and `==` work on scalars only.

## Functions

**Component by component**, on numbers and vectors:

`Abs`, `Sign`, `Min`, `Max`, `Clamp` (ints too), `Floor`, `Ceil`, `Round` (ties to even), `Trunc`, `Frac`, `Sqrt`, `Rsqrt`, `Saturate`, `Radians`, `Degrees`, `Sin`, `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Atan2`, `Exp`, `Exp2`, `Log`, `Log2`, `Log10`, `Pow`, `Step`, `Lerp`, `Unlerp`, `SmoothStep`.

**Vectors:**

`Dot`, `Cross`, `Length`, `LengthSq`, `Distance`, `DistanceSq`, `Normalize`, `NormalizeSafe` (zero instead of NaN), `Reflect`, `Csum`, `Cmin`, `Cmax`.

**Quaternions:**

`Mul`, `Rotate`, `Inverse`, `Conjugate`, `Normalize`, `NormalizeSafe`, `Dot`, `Slerp`, `Nlerp`, `Forward`, `Up`, `Right`, `Angle`.

**Matrices:**

`Mul`, `Transpose`, `Inverse`, `Determinant`, and for `float4x4`, `Transform` (a point) and `Rotate` (a direction).

## Forgiving with bad values

Values can come from other players' input, so math avoids spreading NaN where it can:

- `Math.Clamp` always returns a value in range: NaN gives the lower bound.
- `Math.Min` and `Math.Max` with one NaN argument return the other.
- `Math.NormalizeSafe` gives zero for a zero vector, where `Math.Normalize` would give NaN.

Everywhere else, results match Unity.Mathematics. The transcendental functions (`Sin`, `Exp`, `Pow` and the others) are PurrEngine's own, accurate to about 1 ulp, and give the same bits everywhere.
