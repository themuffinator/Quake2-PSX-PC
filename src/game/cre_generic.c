/*
 * cre_generic.c — the fallback for a creature the port has not transcribed.
 *
 * This supplies decoded animation and action fragments for development. It
 * does not implement arbitrary branch conditions, projectile arguments or
 * special behavior. The seven native creatures no longer rely on this for
 * their frame methods. The full COMMON/ZONE census finds 22 module names;
 * generic callback availability must not be reported as working behavior.
 *
 * ---------------------------------------------------------------------------
 * How it picks an animation without knowing the creature
 * ---------------------------------------------------------------------------
 * It does not guess. The decoder already records WHICH callback installed each
 * move, straight out of the module's own code, so "the move this creature's
 * stand callback installs" is a fact rather than an inference. The first move
 * a callback installs is the one taken; a creature that picks between several
 * at random will always take the same one here. That is one of several known
 * limitations, not a faithful implementation of that callback.
 */
#include "ai.h"
#include "crebind.h"
#include "monster.h"

static bool set_via(q2_monster *m, u32 slot)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    const q2_cre_move *cm;

    if (!b || !b->cre)
        return false;

    cm = q2_creature_move_via(b->cre, slot);
    if (!cm) {
        if (slot < Q2_CRE_CALLBACK_SLOTS)
            q2_cre_actions.move_via_missing[slot]++;
        return false;
    }

    if (slot < Q2_CRE_CALLBACK_SLOTS)
        q2_cre_actions.move_via_set[slot]++;
    return q2_cre_set_move(m, cm->first_frame);
}

void q2_cre_generic_stand(q2_monster *m)  { set_via(m, 0);  }
void q2_cre_generic_idle(q2_monster *m)   { set_via(m, 1);  }
void q2_cre_generic_search(q2_monster *m) { set_via(m, 2);  }
void q2_cre_generic_walk(q2_monster *m)   { set_via(m, 3);  }
void q2_cre_generic_attack(q2_monster *m) { set_via(m, 6);  }
void q2_cre_generic_melee(q2_monster *m)  { set_via(m, 7);  }

void q2_cre_generic_run(q2_monster *m)
{
    /* Standing ground beats running, exactly as every transcribed creature's
     * run callback opens — it is the one branch they all share. */
    if (m->aiflags & Q2_AI_STAND_GROUND) {
        if (set_via(m, 0))
            return;
    }
    if (!set_via(m, 4))
        set_via(m, 0);
}

void q2_cre_generic_pain(q2_monster *m) { set_via(m, 11); }

void q2_cre_generic_die(q2_monster *m)
{
    if (m->dead)
        return;
    m->dead = true;
    set_via(m, 12);
}

/*
 * One trampoline per think index. The frame driver calls a plain
 * `void (*)(edict_t *)`, so the index has to be baked into the function rather
 * than passed — thirty-two one-line functions is the cheapest way to do that
 * without a per-creature table.
 */
void q2_cre_run_think(q2_monster *m, u32 index);

#define TRAMP(n) static void think_##n(q2_monster *m) { q2_cre_run_think(m, n); }
TRAMP(0)  TRAMP(1)  TRAMP(2)  TRAMP(3)  TRAMP(4)  TRAMP(5)  TRAMP(6)  TRAMP(7)
TRAMP(8)  TRAMP(9)  TRAMP(10) TRAMP(11) TRAMP(12) TRAMP(13) TRAMP(14) TRAMP(15)
TRAMP(16) TRAMP(17) TRAMP(18) TRAMP(19) TRAMP(20) TRAMP(21) TRAMP(22) TRAMP(23)
TRAMP(24) TRAMP(25) TRAMP(26) TRAMP(27) TRAMP(28) TRAMP(29) TRAMP(30) TRAMP(31)
#undef TRAMP

const q2_cre_impl q2_cre_generic = {
    NULL,
    {
        q2_cre_generic_stand, q2_cre_generic_idle, q2_cre_generic_search, q2_cre_generic_walk,
        q2_cre_generic_run, NULL, q2_cre_generic_attack, q2_cre_generic_melee,
        NULL, NULL, NULL, q2_cre_generic_pain, q2_cre_generic_die
    },
    {
        think_0,  think_1,  think_2,  think_3,  think_4,  think_5,
        think_6,  think_7,  think_8,  think_9,  think_10, think_11,
        think_12, think_13, think_14, think_15, think_16, think_17,
        think_18, think_19, think_20, think_21, think_22, think_23,
        think_24, think_25, think_26, think_27, think_28, think_29,
        think_30, think_31
    },
    NULL
};
