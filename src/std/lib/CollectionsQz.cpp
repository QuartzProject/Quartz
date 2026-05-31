#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kCollectionsQz = R"qz(package org.quartzlang.collections

/// An insertion-ordered set of `Hash` values. Copies share their elements: it is a struct holding a map.
pub struct Set<T: Hash> {
    items: Map<T, bool> = Map()
}

/// A set with the distinct elements of `items`, in their first order.
pub fn setOf<T: Hash>(items: [T]) -> Set<T> {
    let s = Set::<T>()
    for x in items {
        s.items[x] = true
    }
    return s
}

/// Walks a set in insertion order; adding or removing elements meanwhile traps, as with maps.
pub struct SetIter<T: Hash> {
    items: Map<T, bool>
    cursor: int
}

impl<T: Hash> Iterator for SetIter<T> {
    fn next(self) -> (T, SetIter<T>)? {
        if self.cursor < 0 {
            return null
        }
        return (self.items.keyAt(self.cursor), SetIter(self.items, self.items.cursorNext(self.cursor)))
    }
}

impl<T: Hash> Set<T> {
    pub fn len(self) -> int {
        return self.items.len()
    }

    pub fn isEmpty(self) -> bool {
        return self.items.len() == 0
    }

    pub fn contains(self, x: T) -> bool {
        return self.items.has(x)
    }

    /// Adds `x` at the end unless it is already there; true if it was added.
    pub fn add(self, x: T) -> bool {
        if self.items.has(x) {
            return false
        }
        self.items[x] = true
        return true
    }

    pub fn addAll(self, xs: [T]) {
        for x in xs {
            self.items[x] = true
        }
    }

    /// True if `x` was there.
    pub fn remove(self, x: T) -> bool {
        return self.items.remove(x)
    }

    pub fn clear(self) {
        for x in self.items.keys() {
            self.items.remove(x)
        }
    }

    pub fn toList(self) -> [T] {
        return self.items.keys()
    }

    /// A set that does not share elements with this one.
    pub fn copy(self) -> Set<T> {
        let out = Set::<T>()
        for x in self.items {
            out.items[x] = true
        }
        return out
    }

    pub fn iter(self) -> SetIter<T> {
        return SetIter(self.items, self.items.cursorNext(-1))
    }

    /// This set's elements, then the other's that are new.
    pub fn union(self, other: Set<T>) -> Set<T> {
        let out = self.copy()
        for x in other.items {
            out.items[x] = true
        }
        return out
    }

    pub fn intersect(self, other: Set<T>) -> Set<T> {
        let out = Set::<T>()
        for x in self.items {
            if other.items.has(x) {
                out.items[x] = true
            }
        }
        return out
    }

    pub fn difference(self, other: Set<T>) -> Set<T> {
        let out = Set::<T>()
        for x in self.items {
            if !other.items.has(x) {
                out.items[x] = true
            }
        }
        return out
    }

    /// The elements in exactly one of the two: this set's first, then the other's.
    pub fn symmetricDifference(self, other: Set<T>) -> Set<T> {
        let out = self.difference(other)
        for x in other.items {
            if !self.items.has(x) {
                out.items[x] = true
            }
        }
        return out
    }

    pub fn isSubsetOf(self, other: Set<T>) -> bool {
        if self.items.len() > other.items.len() {
            return false
        }
        for x in self.items {
            if !other.items.has(x) {
                return false
            }
        }
        return true
    }

    pub fn isSupersetOf(self, other: Set<T>) -> bool {
        return other.isSubsetOf(self)
    }

    pub fn isDisjoint(self, other: Set<T>) -> bool {
        for x in self.items {
            if other.items.has(x) {
                return false
            }
        }
        return true
    }
}

/// Equal when they hold the same elements, in any order.
impl<T: Hash> Eq for Set<T> {
    fn eq(self, other: Set<T>) -> bool {
        return self.items.len() == other.items.len() && self.isSubsetOf(other)
    }
}

impl<T: Hash + Show> Show for Set<T> {
    fn show(self) -> str {
        let parts = List::<str>()
        for x in self.items {
            parts.push("{x}")
        }
        return "\{" + parts.join(", ") + "}"
    }
}

/// A double-ended queue on a ring buffer. Copies share their elements.
pub struct Deque<T> {
    buf: [T] = []
    meta: [int] = [0, 0, 0]
}

/// Walks a deque from front to back; pushing or popping meanwhile traps.
pub struct DequeIter<T> {
    deque: Deque<T>
    index: int
    version: int
}

impl<T> Iterator for DequeIter<T> {
    fn next(self) -> (T, DequeIter<T>)? {
        let d = self.deque
        if d.meta[2] != self.version {
            panic("deque changed during iteration")
        }
        if self.index >= d.meta[1] {
            return null
        }
        return (d.buf[(d.meta[0] + self.index) & (d.buf.len() - 1)], DequeIter(d, self.index + 1, self.version))
    }
}

impl<T> Deque<T> {
    pub fn len(self) -> int {
        return self.meta[1]
    }

    pub fn isEmpty(self) -> bool {
        return self.meta[1] == 0
    }

    // Doubles a full ring in place: the wrapped front part is copied after the end, so the elements stay contiguous.
    fn grow(self, x: T) {
        let buf = self.buf
        let cap = buf.len()
        if cap == 0 {
            buf.push(x)
            return
        }
        for i in 0..self.meta[0] {
            buf.push(buf[i])
        }
        while buf.len() < 2 * cap {
            buf.push(x)
        }
    }

    pub fn pushBack(self, x: T) {
        let m = self.meta
        if m[1] == self.buf.len() {
            self.grow(x)
        }
        self.buf[(m[0] + m[1]) & (self.buf.len() - 1)] = x
        m[1] += 1
        m[2] += 1
    }

    pub fn pushFront(self, x: T) {
        let m = self.meta
        if m[1] == self.buf.len() {
            self.grow(x)
        }
        m[0] = (m[0] - 1) & (self.buf.len() - 1)
        self.buf[m[0]] = x
        m[1] += 1
        m[2] += 1
    }

    pub fn popFront(self) -> T? {
        let m = self.meta
        if m[1] == 0 {
            return null
        }
        let x = self.buf[m[0]]
        m[0] = (m[0] + 1) & (self.buf.len() - 1)
        m[1] -= 1
        m[2] += 1
        return x
    }

    pub fn popBack(self) -> T? {
        let m = self.meta
        if m[1] == 0 {
            return null
        }
        m[1] -= 1
        m[2] += 1
        return self.buf[(m[0] + m[1]) & (self.buf.len() - 1)]
    }

    pub fn first(self) -> T? {
        return if self.meta[1] == 0 { null } else { self.buf[self.meta[0]] }
    }

    pub fn last(self) -> T? {
        let m = self.meta
        return if m[1] == 0 { null } else { self.buf[(m[0] + m[1] - 1) & (self.buf.len() - 1)] }
    }

    /// The element `i` places from the front; out of range is a trap.
    pub fn get(self, i: int) -> T {
        if i < 0 || i >= self.meta[1] {
            panic("index {i} out of range for a deque of {self.meta[1]}")
        }
        return self.buf[(self.meta[0] + i) & (self.buf.len() - 1)]
    }

    pub fn set(self, i: int, x: T) {
        if i < 0 || i >= self.meta[1] {
            panic("index {i} out of range for a deque of {self.meta[1]}")
        }
        self.buf[(self.meta[0] + i) & (self.buf.len() - 1)] = x
    }

    pub fn clear(self) {
        self.meta[0] = 0
        self.meta[1] = 0
        self.meta[2] += 1
    }

    pub fn toList(self) -> [T] {
        let out = List::<T>()
        let mask = self.buf.len() - 1
        for i in 0..self.meta[1] {
            out.push(self.buf[(self.meta[0] + i) & mask])
        }
        return out
    }

    pub fn iter(self) -> DequeIter<T> {
        return DequeIter(self, 0, self.meta[2])
    }
}

impl<T: Eq> Eq for Deque<T> {
    fn eq(self, other: Deque<T>) -> bool {
        if self.meta[1] != other.meta[1] {
            return false
        }
        for i in 0..self.meta[1] {
            if self.get(i) != other.get(i) {
                return false
            }
        }
        return true
    }
}

impl<T: Show> Show for Deque<T> {
    fn show(self) -> str {
        return "{self.toList()}"
    }
}

/// A set of non-negative integers kept as bits; it grows to the largest one. Copies share their bits.
pub struct BitSet {
    words: [int] = []
}

fn popCount(x: int) -> int {
    var v = x as u64
    v = v - ((v >> 1) & 0x5555555555555555)
    v = (v & 0x3333333333333333) + ((v >> 2) & 0x3333333333333333)
    v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0F
    return ((v * 0x0101010101010101) >> 56) as int
}

fn trailingZeros(x: int) -> int {
    return popCount((x & -x) - 1)
}

fn checkBit(i: int) {
    if i < 0 {
        panic("bit index {i} is negative")
    }
}

/// Walks the set bits from the lowest.
pub struct BitSetIter {
    words: [int]
    next: int
}

impl Iterator for BitSetIter {
    fn next(self) -> (int, BitSetIter)? {
        let i = nextBit(self.words, self.next)
        return if i < 0 { null } else { (i, BitSetIter(self.words, i + 1)) }
    }
}

fn nextBit(words: [int], from: int) -> int {
    var w = from >> 6
    if w >= words.len() {
        return -1
    }
    var bits = words[w] & (-1 << (from & 63))
    while bits == 0 {
        w += 1
        if w >= words.len() {
            return -1
        }
        bits = words[w]
    }
    return (w << 6) + trailingZeros(bits)
}

impl BitSet {
    fn grow(self, w: int) {
        while self.words.len() <= w {
            self.words.push(0)
        }
    }

    pub fn get(self, i: int) -> bool {
        if i < 0 {
            panic("bit index {i} is negative")
        }
        let w = i >> 6
        return w < self.words.len() && (self.words[w] >> (i & 63)) & 1 != 0
    }

    pub fn set(self, i: int) {
        if i < 0 {
            panic("bit index {i} is negative")
        }
        let w = i >> 6
        if w >= self.words.len() {
            self.grow(w)
        }
        self.words[w] |= 1 << (i & 63)
    }

    pub fn unset(self, i: int) {
        checkBit(i)
        let w = i >> 6
        if w < self.words.len() {
            self.words[w] &= ~(1 << (i & 63))
        }
    }

    pub fn flip(self, i: int) {
        checkBit(i)
        let w = i >> 6
        if w >= self.words.len() {
            self.grow(w)
        }
        self.words[w] ^= 1 << (i & 63)
    }

    /// How many bits are set.
    pub fn count(self) -> int {
        var n = 0
        for w in self.words {
            n += popCount(w)
        }
        return n
    }

    pub fn isEmpty(self) -> bool {
        for w in self.words {
            if w != 0 {
                return false
            }
        }
        return true
    }

    pub fn clear(self) {
        self.words.clear()
    }

    /// The lowest set bit at or after `from`.
    pub fn nextSet(self, from: int) -> int? {
        checkBit(from)
        let i = nextBit(self.words, from)
        return if i < 0 { null } else { i }
    }

    /// The highest set bit.
    pub fn highest(self) -> int? {
        var w = self.words.len() - 1
        while w >= 0 {
            let bits = self.words[w]
            if bits != 0 {
                var b = 63
                while (bits >> b) & 1 == 0 {
                    b -= 1
                }
                return (w << 6) + b
            }
            w -= 1
        }
        return null
    }

    pub fn toList(self) -> [int] {
        let out = List::<int>()
        var i = nextBit(self.words, 0)
        while i >= 0 {
            out.push(i)
            i = nextBit(self.words, i + 1)
        }
        return out
    }

    pub fn iter(self) -> BitSetIter {
        return BitSetIter(self.words, 0)
    }

    fn combine(self, other: BitSet, op: int) -> BitSet {
        let a = self.words
        let b = other.words
        let n = if a.len() > b.len() { a.len() } else { b.len() }
        let out = List::<int>()
        for i in 0..n {
            let x = if i < a.len() { a[i] } else { 0 }
            let y = if i < b.len() { b[i] } else { 0 }
            out.push(match op {
                0 => x | y
                1 => x & y
                2 => x ^ y
                _ => x & ~y
            })
        }
        return BitSet(out)
    }

    /// The bits set in either.
    pub fn union(self, other: BitSet) -> BitSet {
        return self.combine(other, 0)
    }

    /// The bits set in both.
    pub fn intersect(self, other: BitSet) -> BitSet {
        return self.combine(other, 1)
    }

    /// The bits set in exactly one.
    pub fn symmetricDifference(self, other: BitSet) -> BitSet {
        return self.combine(other, 2)
    }

    /// The bits set here and not in `other`.
    pub fn difference(self, other: BitSet) -> BitSet {
        return self.combine(other, 3)
    }

    pub fn intersects(self, other: BitSet) -> bool {
        let n = if self.words.len() < other.words.len() { self.words.len() } else { other.words.len() }
        for i in 0..n {
            if self.words[i] & other.words[i] != 0 {
                return true
            }
        }
        return false
    }
}

/// Equal when the same bits are set, however far each has grown.
impl Eq for BitSet {
    fn eq(self, other: BitSet) -> bool {
        return self.symmetricDifference(other).isEmpty()
    }
}

impl Show for BitSet {
    fn show(self) -> str {
        let parts = List::<str>()
        for i in self {
            parts.push("{i}")
        }
        return "\{" + parts.join(", ") + "}"
    }
}
)qz";

} // namespace quartz::stdlib
