# Architecture decision records

Each record covers one decision: what the problem was, the options I weighed,
what I chose, what it cost, and what would make me change it. They're written
so someone who disagrees can see exactly where they would have chosen
differently.

| # | Decision | Status |
|---|---|---|
| [0001](0001-sequence-after-validation.md) | Assign sequence numbers after validation, before the journal | Accepted, 2026-08 |
| [0002](0002-journal-before-matching.md) | Write the journal before matching, and journal inputs instead of outputs | Accepted, 2026-08 |
| [0003](0003-flat-price-ladder.md) | Index price levels with a flat ladder instead of `std::map` | Accepted, 2026-10 (replaced the map) |

New records copy [template.md](template.md). A record is never edited to
change its decision. A later record supersedes it, and the old one says so at
the top.
