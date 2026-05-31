#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kQueuesQz = R"qz(package org.quartzlang.collections

/// A binary min-heap: `pop` gives the least element by `Ord`. Copies share their elements.
pub struct PriorityQueue<T: Ord> {
    items: [T] = []
}

impl<T: Ord> PriorityQueue<T> {
    pub fn len(self) -> int {
        return self.items.len()
    }

    pub fn isEmpty(self) -> bool {
        return self.items.len() == 0
    }

    pub fn push(self, x: T) {
        let a = self.items
        a.push(x)
        var i = a.len() - 1
        while i > 0 {
            let p = (i - 1) >> 1
            if !(x < a[p]) {
                break
            }
            a[i] = a[p]
            i = p
        }
        a[i] = x
    }

    /// Removes and returns the least element; among equal ones, any.
    pub fn pop(self) -> T? {
        let a = self.items
        let n = a.len()
        if n == 0 {
            return null
        }
        let top = a[0]
        let x = a.pop().unwrap()
        let m = n - 1
        var i = 0
        while true {
            var c = 2 * i + 1
            if c >= m {
                break
            }
            if c + 1 < m && a[c + 1] < a[c] {
                c += 1
            }
            if !(a[c] < x) {
                break
            }
            a[i] = a[c]
            i = c
        }
        if m > 0 {
            a[i] = x
        }
        return top
    }

    pub fn peek(self) -> T? {
        return if self.items.len() == 0 { null } else { self.items[0] }
    }

    pub fn clear(self) {
        self.items.clear()
    }

    /// The elements from least to greatest, leaving the queue as it is.
    pub fn toSortedList(self) -> [T] {
        let out = self.items.slice(0, self.items.len())
        out.sortWith((a, b) => a.cmp(b))
        return out
    }

    /// The elements in heap order, which is not sorted.
    pub fn iter(self) -> [T] {
        return self.items
    }
}

impl<T: Ord + Show> Show for PriorityQueue<T> {
    fn show(self) -> str {
        return "{self.toSortedList()}"
    }
}

/// A binary min-heap ordered by `cmp` (negative when the first argument goes first). Copies share their elements.
pub struct PriorityQueueBy<T> {
    cmp: fn(T, T) -> int
    items: [T] = []
}

impl<T> PriorityQueueBy<T> {
    pub fn len(self) -> int {
        return self.items.len()
    }

    pub fn isEmpty(self) -> bool {
        return self.items.len() == 0
    }

    pub fn push(self, x: T) {
        let a = self.items
        a.push(x)
        var i = a.len() - 1
        while i > 0 {
            let p = (i - 1) >> 1
            if self.cmp(x, a[p]) >= 0 {
                break
            }
            a[i] = a[p]
            i = p
        }
        a[i] = x
    }

    /// Removes and returns the first element by `cmp`; among equal ones, any.
    pub fn pop(self) -> T? {
        let a = self.items
        let n = a.len()
        if n == 0 {
            return null
        }
        let top = a[0]
        let x = a.pop().unwrap()
        let m = n - 1
        var i = 0
        while true {
            var c = 2 * i + 1
            if c >= m {
                break
            }
            if c + 1 < m && self.cmp(a[c + 1], a[c]) < 0 {
                c += 1
            }
            if self.cmp(a[c], x) >= 0 {
                break
            }
            a[i] = a[c]
            i = c
        }
        if m > 0 {
            a[i] = x
        }
        return top
    }

    pub fn peek(self) -> T? {
        return if self.items.len() == 0 { null } else { self.items[0] }
    }

    pub fn clear(self) {
        self.items.clear()
    }

    /// The elements in `cmp` order, leaving the queue as it is.
    pub fn toSortedList(self) -> [T] {
        let out = self.items.slice(0, self.items.len())
        out.sortWith(self.cmp)
        return out
    }

    /// The elements in heap order, which is not sorted.
    pub fn iter(self) -> [T] {
        return self.items
    }
}

impl<T: Show> Show for PriorityQueueBy<T> {
    fn show(self) -> str {
        return "{self.toSortedList()}"
    }
}

/// Keeps the `capacity` most recently used entries: `get` and `set` make an entry the newest, and adding past the
/// capacity drops the oldest. Copies share their entries.
pub struct LruCache<K: Hash, V> {
    capacity: int
    index: Map<K, int> = Map()
    keys: [K] = []
    vals: [V] = []
    prev: [int] = []
    next: [int] = []
    meta: [int] = [-1, -1, -1, 0]
}

/// Walks a cache from the newest entry to the oldest; changing which keys it holds meanwhile traps.
pub struct LruCacheIter<K: Hash, V> {
    cache: LruCache<K, V>
    slot: int
    version: int
}

impl<K: Hash, V> Iterator for LruCacheIter<K, V> {
    fn next(self) -> ((K, V), LruCacheIter<K, V>)? {
        let c = self.cache
        if c.meta[3] != self.version {
            panic("cache changed during iteration")
        }
        if self.slot < 0 {
            return null
        }
        return ((c.keys[self.slot], c.vals[self.slot]), LruCacheIter(c, c.next[self.slot], self.version))
    }
}

impl<K: Hash, V> LruCache<K, V> {
    pub fn len(self) -> int {
        return self.index.len()
    }

    pub fn isEmpty(self) -> bool {
        return self.index.len() == 0
    }

    pub fn has(self, k: K) -> bool {
        return self.index.has(k)
    }

    // meta: newest slot, oldest slot, first free slot, version; slots link newest to oldest through `next`.
    fn unlink(self, s: int) {
        let p = self.prev[s]
        let n = self.next[s]
        if p >= 0 {
            self.next[p] = n
        } else {
            self.meta[0] = n
        }
        if n >= 0 {
            self.prev[n] = p
        } else {
            self.meta[1] = p
        }
    }

    fn pushNewest(self, s: int) {
        let h = self.meta[0]
        self.prev[s] = -1
        self.next[s] = h
        if h >= 0 {
            self.prev[h] = s
        } else {
            self.meta[1] = s
        }
        self.meta[0] = s
    }

    /// The value of `k`, which becomes the newest entry.
    pub fn get(self, k: K) -> V? {
        let found = self.index.get(k)
        if found == null {
            return null
        }
        let s = found.unwrap()
        if self.meta[0] != s {
            self.unlink(s)
            self.pushNewest(s)
        }
        return self.vals[s]
    }

    /// The value of `k` without making it the newest.
    pub fn peek(self, k: K) -> V? {
        let found = self.index.get(k)
        return if found == null { null } else { self.vals[found.unwrap()] }
    }

    /// Adds or replaces `k` as the newest entry; returns the oldest entry if that had to go to make room.
    pub fn set(self, k: K, v: V) -> (K, V)? {
        let found = self.index.get(k)
        if found != null {
            let s = found.unwrap()
            self.vals[s] = v
            if self.meta[0] != s {
                self.unlink(s)
                self.pushNewest(s)
            }
            return null
        }
        if self.capacity < 1 {
            panic("an LruCache needs a capacity of at least 1, got {self.capacity}")
        }
        var evicted: (K, V)? = null
        var s = self.meta[2]
        if self.index.len() >= self.capacity {
            s = self.meta[1]
            evicted = (self.keys[s], self.vals[s])
            self.index.remove(self.keys[s])
            self.unlink(s)
            self.keys[s] = k
            self.vals[s] = v
        } else if s >= 0 {
            self.meta[2] = self.next[s]
            self.keys[s] = k
            self.vals[s] = v
        } else {
            s = self.keys.len()
            self.keys.push(k)
            self.vals.push(v)
            self.prev.push(-1)
            self.next.push(-1)
        }
        self.index[k] = s
        self.pushNewest(s)
        self.meta[3] += 1
        return evicted
    }

    /// True if `k` was there.
    pub fn remove(self, k: K) -> bool {
        let found = self.index.get(k)
        if found == null {
            return false
        }
        let s = found.unwrap()
        self.index.remove(k)
        self.unlink(s)
        self.next[s] = self.meta[2]
        self.meta[2] = s
        self.meta[3] += 1
        return true
    }

    pub fn clear(self) {
        for k in self.index.keys() {
            self.index.remove(k)
        }
        self.keys.clear()
        self.vals.clear()
        self.prev.clear()
        self.next.clear()
        self.meta[0] = -1
        self.meta[1] = -1
        self.meta[2] = -1
        self.meta[3] += 1
    }

    /// The entries from the newest to the oldest.
    pub fn iter(self) -> LruCacheIter<K, V> {
        return LruCacheIter(self, self.meta[0], self.meta[3])
    }
}

impl<K: Hash + Show, V: Show> Show for LruCache<K, V> {
    fn show(self) -> str {
        let parts = List::<str>()
        for e in self {
            let (k, v) = e
            parts.push("{k}: {v}")
        }
        return "\{" + parts.join(", ") + "}"
    }
}
)qz";

} // namespace quartz::stdlib
