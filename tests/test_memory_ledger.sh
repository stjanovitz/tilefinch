#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
checker="$root/benchmarks/check-memory-ledger.sh"
temporary=$(mktemp -d "${TMPDIR:-/tmp}/tilefinch-ledger-test.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

emit_categories() {
  active=$1
  allocations=$2
  frees=$3
  for name in uncategorized dom javascript style resource layout render \
              session navigation; do
    printf 'memory-category name=%s current=%s peak=1 global-peak=1 payload=%s overhead=0 largest=1 active=%s allocs=%s frees=%s high-water=test\n' \
      "$name" "$active" "$active" "$active" "$allocations" "$frees"
  done
}

good="$temporary/good.log"
{
  printf 'memory-categories phase=stable-page current=9 expected=9 global-peak=9 expected-peak=9 external-reserved=1 reconcile=yes\n'
  emit_categories 1 2 1
  printf 'memory-categories phase=teardown current=0 expected=0 global-peak=9 expected-peak=9 external-reserved=0 reconcile=yes\n'
  emit_categories 0 2 2
} > "$good"
"$checker" "$good"

bad_reconcile="$temporary/bad-reconcile.log"
sed 's/phase=stable-page \(.*\)reconcile=yes/phase=stable-page \1reconcile=no/' \
  "$good" > "$bad_reconcile"
if "$checker" "$bad_reconcile" >/dev/null 2>&1; then
  printf 'ledger checker accepted reconcile=no\n' >&2
  exit 1
fi

bad_balance="$temporary/bad-balance.log"
sed 's/active=1 allocs=2 frees=1/active=1 allocs=3 frees=1/' \
  "$good" > "$bad_balance"
if "$checker" "$bad_balance" >/dev/null 2>&1; then
  printf 'ledger checker accepted an allocation/free imbalance\n' >&2
  exit 1
fi

bad_category="$temporary/bad-category.log"
sed 's/name=dom/name=uncategorized/' "$good" > "$bad_category"
if "$checker" "$bad_category" >/dev/null 2>&1; then
  printf 'ledger checker accepted a duplicate/missing category\n' >&2
  exit 1
fi

bad_field="$temporary/bad-field.log"
sed 's/ active=1//' "$good" > "$bad_field"
if "$checker" "$bad_field" >/dev/null 2>&1; then
  printf 'ledger checker accepted a missing category field\n' >&2
  exit 1
fi

printf 'tilefinch-memory-ledger-tests: all checks passed\n'
