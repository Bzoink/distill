# Phase 1 golden contracts

These files hold stable extraction expectations for the breadth fixtures. They
assert fields that should survive a refactor while deliberately ignoring the
fixture server's ephemeral port. The full structured output remains available
with `corpus/check.py --extract --keep-dom DIR`.

The manifest runs these contracts alongside the extract-stage assertions.
