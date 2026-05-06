#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kGeometryQz = R"qz(package org.quartzlang.geometry

import org.quartzlang.math.{PI, atan2, cos, pow, sin, sqrt}

pub struct Vec2 {
    x: float
    y: float
}

pub struct Vec3 {
    x: float
    y: float
    z: float
}

/// `a + (b - a) * t`; `t` outside 0 to 1 extrapolates.
pub fn lerp(a: float, b: float, t: float) -> float {
    return a + (b - a) * t
}

/// Where `value` lies between `a` and `b`, as `t` for `lerp`; 0 when `a == b`.
pub fn inverseLerp(a: float, b: float, value: float) -> float {
    return if a == b { 0.0 } else { (value - a) / (b - a) }
}

/// 0 below `edge0`, 1 above `edge1`, the smooth Hermite curve in between.
pub fn smoothstep(edge0: float, edge1: float, x: float) -> float {
    let t = inverseLerp(edge0, edge1, x).clamp(0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)
}

impl Vec2 {
    pub fn plus(self, o: Vec2) -> Vec2 {
        return Vec2(self.x + o.x, self.y + o.y)
    }

    pub fn minus(self, o: Vec2) -> Vec2 {
        return Vec2(self.x - o.x, self.y - o.y)
    }

    pub fn times(self, s: float) -> Vec2 {
        return Vec2(self.x * s, self.y * s)
    }

    pub fn div(self, s: float) -> Vec2 {
        return Vec2(self.x / s, self.y / s)
    }

    pub fn negate(self) -> Vec2 {
        return Vec2(-self.x, -self.y)
    }

    pub fn dot(self, o: Vec2) -> float {
        return self.x * o.x + self.y * o.y
    }

    /// The z of the 3D cross product: positive when `o` is counterclockwise from this vector.
    pub fn cross(self, o: Vec2) -> float {
        return self.x * o.y - self.y * o.x
    }

    pub fn lengthSquared(self) -> float {
        return self.x * self.x + self.y * self.y
    }

    pub fn length(self) -> float {
        return sqrt(self.lengthSquared())
    }

    pub fn distance(self, o: Vec2) -> float {
        return self.minus(o).length()
    }

    /// Length 1, or the zero vector unchanged.
    pub fn normalize(self) -> Vec2 {
        let l = self.length()
        return if l == 0.0 { self } else { self.div(l) }
    }

    pub fn lerp(self, o: Vec2, t: float) -> Vec2 {
        return Vec2(lerp(self.x, o.x, t), lerp(self.y, o.y, t))
    }

    /// Radians from the x axis, in -PI to PI.
    pub fn angle(self) -> float {
        return atan2(self.y, self.x)
    }

    /// Counterclockwise by `radians`.
    pub fn rotate(self, radians: float) -> Vec2 {
        let c = cos(radians)
        let s = sin(radians)
        return Vec2(self.x * c - self.y * s, self.x * s + self.y * c)
    }

    pub fn equals(self, o: Vec2) -> bool {
        return self.x == o.x && self.y == o.y
    }

    pub fn toStr(self) -> str {
        return "({self.x}, {self.y})"
    }
}

impl Vec3 {
    pub fn plus(self, o: Vec3) -> Vec3 {
        return Vec3(self.x + o.x, self.y + o.y, self.z + o.z)
    }

    pub fn minus(self, o: Vec3) -> Vec3 {
        return Vec3(self.x - o.x, self.y - o.y, self.z - o.z)
    }

    pub fn times(self, s: float) -> Vec3 {
        return Vec3(self.x * s, self.y * s, self.z * s)
    }

    pub fn div(self, s: float) -> Vec3 {
        return Vec3(self.x / s, self.y / s, self.z / s)
    }

    /// Component by component.
    pub fn scale(self, o: Vec3) -> Vec3 {
        return Vec3(self.x * o.x, self.y * o.y, self.z * o.z)
    }

    pub fn negate(self) -> Vec3 {
        return Vec3(-self.x, -self.y, -self.z)
    }

    pub fn dot(self, o: Vec3) -> float {
        return self.x * o.x + self.y * o.y + self.z * o.z
    }

    pub fn cross(self, o: Vec3) -> Vec3 {
        return Vec3(self.y * o.z - self.z * o.y, self.z * o.x - self.x * o.z, self.x * o.y - self.y * o.x)
    }

    pub fn lengthSquared(self) -> float {
        return self.x * self.x + self.y * self.y + self.z * self.z
    }

    pub fn length(self) -> float {
        return sqrt(self.lengthSquared())
    }

    pub fn distance(self, o: Vec3) -> float {
        return self.minus(o).length()
    }

    pub fn distanceSquared(self, o: Vec3) -> float {
        return self.minus(o).lengthSquared()
    }

    /// Length 1, or the zero vector unchanged.
    pub fn normalize(self) -> Vec3 {
        let l = self.length()
        return if l == 0.0 { self } else { self.div(l) }
    }

    pub fn lerp(self, o: Vec3, t: float) -> Vec3 {
        return Vec3(lerp(self.x, o.x, t), lerp(self.y, o.y, t), lerp(self.z, o.z, t))
    }

    pub fn floor(self) -> Vec3 {
        return Vec3(self.x.floor(), self.y.floor(), self.z.floor())
    }

    /// Right-handed rotations by `radians` around each axis.
    pub fn rotateX(self, radians: float) -> Vec3 {
        let c = cos(radians)
        let s = sin(radians)
        return Vec3(self.x, self.y * c - self.z * s, self.y * s + self.z * c)
    }

    pub fn rotateY(self, radians: float) -> Vec3 {
        let c = cos(radians)
        let s = sin(radians)
        return Vec3(self.x * c + self.z * s, self.y, self.z * c - self.x * s)
    }

    pub fn rotateZ(self, radians: float) -> Vec3 {
        let c = cos(radians)
        let s = sin(radians)
        return Vec3(self.x * c - self.y * s, self.x * s + self.y * c, self.z)
    }

    pub fn equals(self, o: Vec3) -> bool {
        return self.x == o.x && self.y == o.y && self.z == o.z
    }

    pub fn toStr(self) -> str {
        return "({self.x}, {self.y}, {self.z})"
    }
}

// Robert Penner's easing curves, as easings.net writes them: t in 0 to 1 gives 0 to 1 (Back and Elastic overshoot).

pub fn easeInQuad(t: float) -> float {
    return t * t
}

pub fn easeOutQuad(t: float) -> float {
    return 1.0 - (1.0 - t) * (1.0 - t)
}

pub fn easeInOutQuad(t: float) -> float {
    return if t < 0.5 { 2.0 * t * t } else { 1.0 - pow(-2.0 * t + 2.0, 2.0) / 2.0 }
}

pub fn easeInCubic(t: float) -> float {
    return t * t * t
}

pub fn easeOutCubic(t: float) -> float {
    return 1.0 - pow(1.0 - t, 3.0)
}

pub fn easeInOutCubic(t: float) -> float {
    return if t < 0.5 { 4.0 * t * t * t } else { 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0 }
}

pub fn easeInSine(t: float) -> float {
    return 1.0 - cos(t * PI / 2.0)
}

pub fn easeOutSine(t: float) -> float {
    return sin(t * PI / 2.0)
}

pub fn easeInOutSine(t: float) -> float {
    return -(cos(PI * t) - 1.0) / 2.0
}

pub fn easeInExpo(t: float) -> float {
    return if t == 0.0 { 0.0 } else { pow(2.0, 10.0 * t - 10.0) }
}

pub fn easeOutExpo(t: float) -> float {
    return if t == 1.0 { 1.0 } else { 1.0 - pow(2.0, -10.0 * t) }
}

pub fn easeInOutExpo(t: float) -> float {
    if t == 0.0 || t == 1.0 {
        return t
    }
    return if t < 0.5 { pow(2.0, 20.0 * t - 10.0) / 2.0 } else { (2.0 - pow(2.0, -20.0 * t + 10.0)) / 2.0 }
}

const BackC1: float = 1.70158
const BackC2: float = 1.70158 * 1.525
const BackC3: float = 1.70158 + 1.0

pub fn easeInBack(t: float) -> float {
    return BackC3 * t * t * t - BackC1 * t * t
}

pub fn easeOutBack(t: float) -> float {
    return 1.0 + BackC3 * pow(t - 1.0, 3.0) + BackC1 * pow(t - 1.0, 2.0)
}

pub fn easeInOutBack(t: float) -> float {
    return if t < 0.5 {
        (pow(2.0 * t, 2.0) * ((BackC2 + 1.0) * 2.0 * t - BackC2)) / 2.0
    } else {
        (pow(2.0 * t - 2.0, 2.0) * ((BackC2 + 1.0) * (t * 2.0 - 2.0) + BackC2) + 2.0) / 2.0
    }
}

pub fn easeInElastic(t: float) -> float {
    if t == 0.0 || t == 1.0 {
        return t
    }
    return -pow(2.0, 10.0 * t - 10.0) * sin((t * 10.0 - 10.75) * (2.0 * PI / 3.0))
}

pub fn easeOutElastic(t: float) -> float {
    if t == 0.0 || t == 1.0 {
        return t
    }
    return pow(2.0, -10.0 * t) * sin((t * 10.0 - 0.75) * (2.0 * PI / 3.0)) + 1.0
}

pub fn easeInOutElastic(t: float) -> float {
    if t == 0.0 || t == 1.0 {
        return t
    }
    let c5 = 2.0 * PI / 4.5
    return if t < 0.5 {
        -(pow(2.0, 20.0 * t - 10.0) * sin((20.0 * t - 11.125) * c5)) / 2.0
    } else {
        (pow(2.0, -20.0 * t + 10.0) * sin((20.0 * t - 11.125) * c5)) / 2.0 + 1.0
    }
}

pub fn easeOutBounce(t: float) -> float {
    let n1 = 7.5625
    let d1 = 2.75
    if t < 1.0 / d1 {
        return n1 * t * t
    }
    if t < 2.0 / d1 {
        let u = t - 1.5 / d1
        return n1 * u * u + 0.75
    }
    if t < 2.5 / d1 {
        let u = t - 2.25 / d1
        return n1 * u * u + 0.9375
    }
    let u = t - 2.625 / d1
    return n1 * u * u + 0.984375
}

pub fn easeInBounce(t: float) -> float {
    return 1.0 - easeOutBounce(1.0 - t)
}

pub fn easeInOutBounce(t: float) -> float {
    return if t < 0.5 { (1.0 - easeOutBounce(1.0 - 2.0 * t)) / 2.0 } else { (1.0 + easeOutBounce(2.0 * t - 1.0)) / 2.0 }
}
)qz";

const std::string_view kNoiseQz = R"qz(package org.quartzlang.noise

import org.quartzlang.random.Random

/// Ken Perlin's improved noise (2002) over a permutation shuffled from `seed`: smooth values in about -1 to 1, the same
/// for a seed on every platform and in both tiers. `noise2` is `noise3` at z = 0.
pub struct Perlin {
    perm: [int]
}

pub fn perlin(seed: int) -> Perlin {
    var p: [int] = []
    for i in 0..256 {
        p.push(i)
    }
    Random(seed).shuffle(p)
    for i in 0..256 {
        p.push(p[i])
    }
    return Perlin(p)
}

fn fade(t: float) -> float {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)
}

fn mix(t: float, a: float, b: float) -> float {
    return a + t * (b - a)
}

fn grad(hash: int, x: float, y: float, z: float) -> float {
    let h = hash & 15
    let u = if h < 8 { x } else { y }
    let v = if h < 4 { y } else if h == 12 || h == 14 { x } else { z }
    return (if (h & 1) == 0 { u } else { -u }) + (if (h & 2) == 0 { v } else { -v })
}

impl Perlin {
    pub fn noise3(self, x: float, y: float, z: float) -> float {
        let p = self.perm
        let fx = x.floor()
        let fy = y.floor()
        let fz = z.floor()
        let xi = (fx as int) & 255
        let yi = (fy as int) & 255
        let zi = (fz as int) & 255
        let xf = x - fx
        let yf = y - fy
        let zf = z - fz
        let u = fade(xf)
        let v = fade(yf)
        let w = fade(zf)
        let a = p[xi] + yi
        let aa = p[a] + zi
        let ab = p[a + 1] + zi
        let b = p[xi + 1] + yi
        let ba = p[b] + zi
        let bb = p[b + 1] + zi
        let x1 = mix(u, grad(p[aa], xf, yf, zf), grad(p[ba], xf - 1.0, yf, zf))
        let x2 = mix(u, grad(p[ab], xf, yf - 1.0, zf), grad(p[bb], xf - 1.0, yf - 1.0, zf))
        let x3 = mix(u, grad(p[aa + 1], xf, yf, zf - 1.0), grad(p[ba + 1], xf - 1.0, yf, zf - 1.0))
        let x4 = mix(u, grad(p[ab + 1], xf, yf - 1.0, zf - 1.0), grad(p[bb + 1], xf - 1.0, yf - 1.0, zf - 1.0))
        return mix(w, mix(v, x1, x2), mix(v, x3, x4))
    }

    pub fn noise2(self, x: float, y: float) -> float {
        return self.noise3(x, y, 0.0)
    }

    /// Fractal sum of `octaves` layers, each `lacunarity` times the frequency and `gain` times the amplitude of the
    /// last, divided by the total amplitude.
    pub fn fbm3(self, x: float, y: float, z: float, octaves: int = 4, lacunarity: float = 2.0, gain: float = 0.5) -> float {
        var sum = 0.0
        var amplitude = 1.0
        var frequency = 1.0
        var total = 0.0
        for i in 0..octaves {
            sum += amplitude * self.noise3(x * frequency, y * frequency, z * frequency)
            total += amplitude
            amplitude *= gain
            frequency *= lacunarity
        }
        return if total == 0.0 { 0.0 } else { sum / total }
    }
}
)qz";

} // namespace quartz::stdlib
