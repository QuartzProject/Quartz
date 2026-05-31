#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kSortedQz = R"qz(package org.quartzlang.collections

// Chunks hold between kLoad / 4 and 2 * kLoad keys: a lookup searches the chunk ends, then one chunk.
const kLoad = 128

/// The first index in the sorted `xs` whose element is not less than `k`.
fn lowerBound<K: Ord>(xs: [K], k: K) -> int {
    var lo = 0
    var hi = xs.len()
    while lo < hi {
        let mid = (lo + hi) >> 1
        if xs[mid] < k {
            lo = mid + 1
        } else {
            hi = mid
        }
    }
    return lo
}

/// The first index in the sorted `xs` whose element is greater than `k`.
fn upperBound<K: Ord>(xs: [K], k: K) -> int {
    var lo = 0
    var hi = xs.len()
    while lo < hi {
        let mid = (lo + hi) >> 1
        if k < xs[mid] {
            hi = mid
        } else {
            lo = mid + 1
        }
    }
    return lo
}

/// A map ordered by its keys. Copies share their entries: it is a struct holding lists.
pub struct SortedMap<K: Ord, V> {
    keys: [[K]] = []
    vals: [[V]] = []
    lasts: [K] = []
    meta: [int] = [0, 0]
}

/// Walks a sorted map in key order between two positions; adding or removing keys meanwhile traps.
pub struct SortedMapIter<K: Ord, V> {
    map: SortedMap<K, V>
    chunk: int
    index: int
    endChunk: int
    endIndex: int
    version: int
}

impl<K: Ord, V> Iterator for SortedMapIter<K, V> {
    fn next(self) -> ((K, V), SortedMapIter<K, V>)? {
        let m = self.map
        if m.meta[1] != self.version {
            panic("sorted map changed during iteration")
        }
        if self.chunk == self.endChunk && self.index == self.endIndex {
            return null
        }
        let ks = m.keys[self.chunk]
        let item = (ks[self.index], m.vals[self.chunk][self.index])
        var c = self.chunk
        var i = self.index + 1
        if i == ks.len() {
            c += 1
            i = 0
        }
        return (item, SortedMapIter(m, c, i, self.endChunk, self.endIndex, self.version))
    }
}

impl<K: Ord, V> SortedMap<K, V> {
    pub fn len(self) -> int {
        return self.meta[0]
    }

    pub fn isEmpty(self) -> bool {
        return self.meta[0] == 0
    }

    pub fn get(self, k: K) -> V? {
        let c = lowerBound(self.lasts, k)
        if c == self.lasts.len() {
            return null
        }
        let ks = self.keys[c]
        let i = lowerBound(ks, k)
        return if ks[i] == k { self.vals[c][i] } else { null }
    }

    pub fn has(self, k: K) -> bool {
        let c = lowerBound(self.lasts, k)
        if c == self.lasts.len() {
            return false
        }
        let ks = self.keys[c]
        return ks[lowerBound(ks, k)] == k
    }

    /// Adds `k` or replaces its value.
    pub fn set(self, k: K, v: V) {
        let n = self.lasts.len()
        if n == 0 {
            self.keys.push([k])
            self.vals.push([v])
            self.lasts.push(k)
            self.meta[0] = 1
            self.meta[1] += 1
            return
        }
        var c = lowerBound(self.lasts, k)
        if c == n {
            c = n - 1
        }
        let ks = self.keys[c]
        let i = lowerBound(ks, k)
        if i < ks.len() && ks[i] == k {
            self.vals[c][i] = v
            return
        }
        ks.insert(i, k)
        self.vals[c].insert(i, v)
        if i == ks.len() - 1 {
            self.lasts[c] = k
        }
        self.meta[0] += 1
        self.meta[1] += 1
        if ks.len() > 2 * kLoad {
            self.split(c)
        }
    }

    fn split(self, c: int) {
        let ks = self.keys[c]
        let vs = self.vals[c]
        let half = ks.len() >> 1
        self.keys[c] = ks.slice(0, half)
        self.vals[c] = vs.slice(0, half)
        self.keys.insert(c + 1, ks.slice(half, ks.len()))
        self.vals.insert(c + 1, vs.slice(half, vs.len()))
        self.lasts.insert(c, ks[half - 1])
    }

    /// True if `k` was there.
    pub fn remove(self, k: K) -> bool {
        let c = lowerBound(self.lasts, k)
        if c == self.lasts.len() {
            return false
        }
        let ks = self.keys[c]
        let i = lowerBound(ks, k)
        if ks[i] != k {
            return false
        }
        self.removeAt(c, i)
        return true
    }

    fn removeAt(self, c: int, i: int) {
        let ks = self.keys[c]
        ks.removeAt(i)
        self.vals[c].removeAt(i)
        self.meta[0] -= 1
        self.meta[1] += 1
        if ks.len() == 0 {
            self.keys.removeAt(c)
            self.vals.removeAt(c)
            self.lasts.removeAt(c)
            return
        }
        if i == ks.len() {
            self.lasts[c] = ks[i - 1]
        }
        if ks.len() < kLoad / 4 && self.lasts.len() > 1 {
            let left = if c + 1 < self.lasts.len() { c } else { c - 1 }
            let a = self.keys[left]
            a.insertAll(a.len(), self.keys[left + 1])
            self.vals[left].insertAll(self.vals[left].len(), self.vals[left + 1])
            self.lasts[left] = self.lasts[left + 1]
            self.keys.removeAt(left + 1)
            self.vals.removeAt(left + 1)
            self.lasts.removeAt(left + 1)
            if a.len() > 2 * kLoad {
                self.split(left)
            }
        }
    }

    pub fn clear(self) {
        self.keys.clear()
        self.vals.clear()
        self.lasts.clear()
        self.meta[0] = 0
        self.meta[1] += 1
    }

    fn entry(self, c: int, i: int) -> (K, V) {
        return (self.keys[c][i], self.vals[c][i])
    }

    pub fn first(self) -> (K, V)? {
        return if self.meta[0] == 0 { null } else { self.entry(0, 0) }
    }

    pub fn last(self) -> (K, V)? {
        let c = self.lasts.len() - 1
        return if c < 0 { null } else { self.entry(c, self.keys[c].len() - 1) }
    }

    pub fn firstKey(self) -> K? {
        return if self.meta[0] == 0 { null } else { self.keys[0][0] }
    }

    pub fn lastKey(self) -> K? {
        let c = self.lasts.len() - 1
        return if c < 0 { null } else { self.lasts[c] }
    }

    /// The position of the first key not less than `k` (or, with `after`, greater than it), as (chunk, index).
    fn seek(self, k: K, after: bool) -> (int, int) {
        let c = if after { upperBound(self.lasts, k) } else { lowerBound(self.lasts, k) }
        if c == self.lasts.len() {
            return (c, 0)
        }
        return (c, if after { upperBound(self.keys[c], k) } else { lowerBound(self.keys[c], k) })
    }

    fn entryAt(self, p: (int, int)) -> (K, V)? {
        let (c, i) = p
        return if c < self.lasts.len() { self.entry(c, i) } else { null }
    }

    fn before(self, p: (int, int)) -> (K, V)? {
        let (c, i) = p
        if i > 0 {
            return self.entry(c, i - 1)
        }
        return if c > 0 { self.entry(c - 1, self.keys[c - 1].len() - 1) } else { null }
    }

    /// The entry with the greatest key not greater than `k`.
    pub fn floor(self, k: K) -> (K, V)? {
        return self.before(self.seek(k, true))
    }

    /// The entry with the least key not less than `k`.
    pub fn ceiling(self, k: K) -> (K, V)? {
        return self.entryAt(self.seek(k, false))
    }

    /// The entry with the greatest key less than `k`.
    pub fn lower(self, k: K) -> (K, V)? {
        return self.before(self.seek(k, false))
    }

    /// The entry with the least key greater than `k`.
    pub fn higher(self, k: K) -> (K, V)? {
        return self.entryAt(self.seek(k, true))
    }

    pub fn popFirst(self) -> (K, V)? {
        if self.meta[0] == 0 {
            return null
        }
        let e = self.entry(0, 0)
        self.removeAt(0, 0)
        return e
    }

    pub fn popLast(self) -> (K, V)? {
        let c = self.lasts.len() - 1
        if c < 0 {
            return null
        }
        let i = self.keys[c].len() - 1
        let e = self.entry(c, i)
        self.removeAt(c, i)
        return e
    }

    pub fn keys(self) -> [K] {
        let out = List::<K>()
        for ks in self.keys {
            out.insertAll(out.len(), ks)
        }
        return out
    }

    pub fn values(self) -> [V] {
        let out = List::<V>()
        for vs in self.vals {
            out.insertAll(out.len(), vs)
        }
        return out
    }

    /// Every entry in key order.
    pub fn iter(self) -> SortedMapIter<K, V> {
        return SortedMapIter(self, 0, 0, self.lasts.len(), 0, self.meta[1])
    }

    /// The entries with `from <= key < until`, in key order.
    pub fn range(self, from: K, until: K) -> SortedMapIter<K, V> {
        let (c, i) = self.seek(from, false)
        var (ec, ei) = self.seek(until, false)
        if ec < c || (ec == c && ei < i) {
            ec = c
            ei = i
        }
        return SortedMapIter(self, c, i, ec, ei, self.meta[1])
    }

    /// The entries with `key >= from`, in key order.
    pub fn rangeFrom(self, from: K) -> SortedMapIter<K, V> {
        let (c, i) = self.seek(from, false)
        return SortedMapIter(self, c, i, self.lasts.len(), 0, self.meta[1])
    }

    /// The entries with `key < until`, in key order.
    pub fn rangeUntil(self, until: K) -> SortedMapIter<K, V> {
        let (c, i) = self.seek(until, false)
        return SortedMapIter(self, 0, 0, c, i, self.meta[1])
    }
}

impl<K: Ord, V: Eq> Eq for SortedMap<K, V> {
    fn eq(self, other: SortedMap<K, V>) -> bool {
        if self.meta[0] != other.meta[0] {
            return false
        }
        let ks = other.keys()
        let vs = other.values()
        for i, e in self {
            let (k, v) = e
            if k != ks[i] || v != vs[i] {
                return false
            }
        }
        return true
    }
}

impl<K: Ord + Show, V: Show> Show for SortedMap<K, V> {
    fn show(self) -> str {
        let parts = List::<str>()
        for e in self {
            let (k, v) = e
            parts.push("{k}: {v}")
        }
        return "\{" + parts.join(", ") + "}"
    }
}

/// A set ordered by `Ord`. Copies share their elements: it is a struct holding lists.
pub struct SortedSet<T: Ord> {
    items: SortedMap<T, bool> = SortedMap()
}

/// Walks a sorted set in order; adding or removing elements meanwhile traps.
pub struct SortedSetIter<T: Ord> {
    inner: SortedMapIter<T, bool>
}

impl<T: Ord> Iterator for SortedSetIter<T> {
    fn next(self) -> (T, SortedSetIter<T>)? {
        if let step = self.inner.next() {
            let (e, rest) = step
            return (e.0, SortedSetIter(rest))
        }
        return null
    }
}

fn keyOf<T>(e: (T, bool)?) -> T? {
    if let x = e {
        return x.0
    }
    return null
}

/// A sorted set with the elements of `items`.
pub fn sortedSetOf<T: Ord>(items: [T]) -> SortedSet<T> {
    let s = SortedSet::<T>()
    for x in items {
        s.items.set(x, true)
    }
    return s
}

impl<T: Ord> SortedSet<T> {
    pub fn len(self) -> int {
        return self.items.len()
    }

    pub fn isEmpty(self) -> bool {
        return self.items.len() == 0
    }

    pub fn contains(self, x: T) -> bool {
        return self.items.has(x)
    }

    /// True if `x` was not there yet.
    pub fn add(self, x: T) -> bool {
        let before = self.items.len()
        self.items.set(x, true)
        return self.items.len() != before
    }

    /// True if `x` was there.
    pub fn remove(self, x: T) -> bool {
        return self.items.remove(x)
    }

    pub fn clear(self) {
        self.items.clear()
    }

    pub fn first(self) -> T? {
        return self.items.firstKey()
    }

    pub fn last(self) -> T? {
        return self.items.lastKey()
    }

    /// The greatest element not greater than `x`.
    pub fn floor(self, x: T) -> T? {
        return keyOf(self.items.floor(x))
    }

    /// The least element not less than `x`.
    pub fn ceiling(self, x: T) -> T? {
        return keyOf(self.items.ceiling(x))
    }

    /// The greatest element less than `x`.
    pub fn lower(self, x: T) -> T? {
        return keyOf(self.items.lower(x))
    }

    /// The least element greater than `x`.
    pub fn higher(self, x: T) -> T? {
        return keyOf(self.items.higher(x))
    }

    pub fn popFirst(self) -> T? {
        return keyOf(self.items.popFirst())
    }

    pub fn popLast(self) -> T? {
        return keyOf(self.items.popLast())
    }

    pub fn toList(self) -> [T] {
        return self.items.keys()
    }

    pub fn iter(self) -> SortedSetIter<T> {
        return SortedSetIter(self.items.iter())
    }

    /// The elements with `from <= x < until`, in order.
    pub fn range(self, from: T, until: T) -> SortedSetIter<T> {
        return SortedSetIter(self.items.range(from, until))
    }

    /// The elements not less than `from`, in order.
    pub fn rangeFrom(self, from: T) -> SortedSetIter<T> {
        return SortedSetIter(self.items.rangeFrom(from))
    }

    /// The elements less than `until`, in order.
    pub fn rangeUntil(self, until: T) -> SortedSetIter<T> {
        return SortedSetIter(self.items.rangeUntil(until))
    }
}

impl<T: Ord> Eq for SortedSet<T> {
    fn eq(self, other: SortedSet<T>) -> bool {
        return self.items == other.items
    }
}

impl<T: Ord + Show> Show for SortedSet<T> {
    fn show(self) -> str {
        let parts = List::<str>()
        for x in self {
            parts.push("{x}")
        }
        return "\{" + parts.join(", ") + "}"
    }
}
)qz";

} // namespace quartz::stdlib
