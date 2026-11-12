# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""A tiny deterministic generator (splitmix64). Python only guarantees the sequence of random.random() across versions, not choice/randint/shuffle,
so everything here is built on our own 64-bit integer output: the same seed gives the same numbers on every Python version and platform."""

MASK = (1 << 64) - 1


def mix64(z):
    """The splitmix64 finaliser: a bijective scramble of a 64-bit integer."""
    z &= MASK
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
    return z ^ (z >> 31)


def fnv1a64(text):
    h = 0xCBF29CE484222325
    for b in text.encode("utf-8"):
        h = ((h ^ b) * 0x100000001B3) & MASK
    return h


class Rng:
    def __init__(self, seed=0):
        self.state = int(seed) & MASK

    @classmethod
    def for_slot(cls, seed, bar, role):
        """A generator for one (seed, bar, role) slot, independent of every other slot."""
        s = mix64((int(seed) & MASK) ^ 0x9E3779B97F4A7C15)
        s = mix64(s ^ (int(bar) & MASK))
        return cls(mix64(s ^ fnv1a64(role)))

    def next_u64(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK
        return mix64(self.state)

    def random(self):
        """A float in [0, 1) with 53 bits."""
        return (self.next_u64() >> 11) / float(1 << 53)

    def below(self, n):
        """An integer in [0, n), by the multiply-shift method (bias below 2**-60 for the small n used here)."""
        if n <= 0:
            raise ValueError("n must be positive")
        return (self.next_u64() * n) >> 64

    def randint(self, a, b):
        """An integer in [a, b], both ends included."""
        return a + self.below(b - a + 1)

    def choice(self, seq):
        if not seq:
            raise IndexError("choice from an empty sequence")
        return seq[self.below(len(seq))]

    def shuffle(self, items):
        """In-place Fisher-Yates shuffle."""
        for i in range(len(items) - 1, 0, -1):
            j = self.below(i + 1)
            items[i], items[j] = items[j], items[i]
