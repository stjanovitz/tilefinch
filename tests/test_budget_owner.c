#include "tilefinch/budget.h"

#include <stdio.h>

#if !defined(TILEFINCH_OWNER_CHECKS)
int main(void)
{
    puts("budget owner checks: SKIP (TILEFINCH_OWNER_CHECKS is off)");
    return 0;
}
#else

#include <pthread.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

typedef struct {
    Budget *budget;
    bool adopt;
    void *allocation;
} ForeignThread;

static void *foreign_allocate(void *opaque)
{
    ForeignThread *foreign = opaque;
    if (foreign->adopt) budget_adopt_current_thread(foreign->budget);
    foreign->allocation = budget_malloc(foreign->budget, 32);
    return NULL;
}

static int run_foreign(ForeignThread *foreign)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, foreign_allocate, foreign) != 0)
        return 1;
    return pthread_join(thread, NULL);
}

int main(void)
{
    /* This test provokes violations on purpose; every other test binary
       keeps the default, where the first one aborts. */
    budget_owner_checks_set_fatal(false);

    Budget budget;
    budget_init(&budget, 64u * 1024u);
    void *mine = budget_malloc(&budget, 16);
    CHECK(mine != NULL);
    mine = budget_realloc(&budget, mine, 64);
    CHECK(mine != NULL);
    CHECK(budget_owner_violations() == 0);

    /* A second thread touching the unlocked ledger is the defect the rule
       exists to prevent. The operation still completes: the check reports,
       it does not change allocator behaviour. */
    ForeignThread intruder = { .budget = &budget };
    CHECK(run_foreign(&intruder) == 0);
    CHECK(intruder.allocation != NULL);
    CHECK(budget_owner_violations() == 1);

    /* A deliberate handoff is not a violation, and moves ownership. */
    ForeignThread heir = { .budget = &budget, .adopt = true };
    CHECK(run_foreign(&heir) == 0);
    CHECK(heir.allocation != NULL);
    CHECK(budget_owner_violations() == 1);
    budget_free(&budget, mine);
    CHECK(budget_owner_violations() == 2);

    /* Re-initialization forgets the owner; the next toucher binds. */
    budget_adopt_current_thread(&budget);
    budget_free(&budget, intruder.allocation);
    budget_free(&budget, heir.allocation);
    CHECK(budget.current == 0);
    budget_init(&budget, 1024u);
    ForeignThread first = { .budget = &budget };
    CHECK(run_foreign(&first) == 0);
    CHECK(first.allocation != NULL);
    CHECK(budget_owner_violations() == 2);
    budget_adopt_current_thread(&budget);
    budget_free(&budget, first.allocation);
    CHECK(budget.current == 0 && budget_owner_violations() == 2);

    puts("budget owner checks: PASS");
    return 0;
}
#endif
