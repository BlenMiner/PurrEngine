// Which systems in the tick could run at the same time, and why the others
// wait. The tick doesn't run on threads yet; this is the plan it will follow,
// shown in editors (above each system, and on hover) and by `tidec --schedule`
// so game code can be written for it now.

#include "ast.h"

// The archetypes a system runs on, as a bitset.
typedef struct arch_set {
    uint64_t words[4]; // Up to 256 archetypes
} arch_set;

static arch_set archetypes_of(const program *prog, const decl *sys)
{
    arch_set set = {{0}};
    for (int a = 0; a < prog->archetypes.count && a < 256; a++) {
        const uint64_t mask = prog->archetypes.items[a];
        if (prog->archetype_local.items[a] != sys->entity_local) continue; // Another world's
        if ((mask & sys->need_mask) == sys->need_mask && !(mask & sys->without_mask)) {
            set.words[a / 64] |= (uint64_t)1 << (a % 64);
        }
    }
    return set;
}

static bool share_archetypes(const arch_set *a, const arch_set *b)
{
    for (int i = 0; i < 4; i++) {
        if (a->words[i] & b->words[i]) return true;
    }
    return false;
}

static void add_conflict(system_wait *w, const conflict_kind kind)
{
    const conflict c = {NULL, kind};
    vec_push(w->conflicts, c);
}

int parallel_loops(const stmt *s)
{
    if (!s) return 0;
    if (s->kind == S_PARALLEL || (s->kind == S_FOREACH && s->value->type.kind == TY_GRID && s->parallel)) return 1;
    int count = 0;
    for (int i = 0; i < s->stmts.count; i++) count += parallel_loops(s->stmts.items[i]);
    for (int i = 0; i < s->cases.count; i++) {
        for (int k = 0; k < s->cases.items[i].body.count; k++) count += parallel_loops(s->cases.items[i].body.items[k]);
    }
    return count + parallel_loops(s->init) + parallel_loops(s->step) + parallel_loops(s->then_stmt)
         + parallel_loops(s->else_stmt);
}

bool system_splits(const decl *sys)
{
    if (sys->is_view || !sys->per_entity || sys->writes_text) return false;
    for (int i = 0; i < sys->params.count; i++) {
        if (sys->params.items[i].type.kind == TY_SINGLETON && sys->params.items[i].mode == PARAM_MUT) return false;
    }
    return true;
}

// The components and singletons two systems both use, where at least one of
// them writes. Components only count if the systems can meet the same entity.
// What's the whole world's counts whatever entities they meet: changing its
// heap (reading it alongside is fine), and the entity IDs spawns hand out,
// which a system hands out as it runs unless it splits (its spawns get theirs
// once it's done, in the tick's order, without waiting). C is trusted, so
// calling it makes no system wait.
// The component or singleton a parameter reads or writes, or NULL.
static const decl *accessed(const param *p)
{
    if (p->mode != PARAM_READ && p->mode != PARAM_MUT) return NULL;
    return p->type.kind == TY_COMPONENT || p->type.kind == TY_SINGLETON ? p->type.decl : NULL;
}

static void find_conflicts(const decl *earlier, const decl *later, const bool same_entities, system_wait *w)
{
    // Starting a task runs its code until it first waits, which can spawn and
    // change text, and the match keeps its tasks in the order they start.
    if (earlier->starts_tasks || later->starts_tasks) {
        if ((earlier->writes_text || earlier->spawns) && (later->writes_text || later->spawns)) add_conflict(w, CONFLICT_TASKS);
    } else {
        if (earlier->writes_text && later->writes_text) add_conflict(w, CONFLICT_TEXT);
        if (earlier->spawns && later->spawns && !system_splits(later)) add_conflict(w, CONFLICT_SPAWN);
    }
    for (int i = 0; i < earlier->params.count; i++) {
        const param *a = &earlier->params.items[i];
        const decl *data = accessed(a);
        if (!data) continue;
        if (data->kind == DECL_COMPONENT && !same_entities) continue;
        for (int k = 0; k < later->params.count; k++) {
            const param *b = &later->params.items[k];
            if (accessed(b) != data) continue;
            const bool a_writes = a->mode == PARAM_MUT;
            const bool b_writes = b->mode == PARAM_MUT;
            if (!a_writes && !b_writes) continue;
            const conflict c = {(decl *)data, a_writes && b_writes ? CONFLICT_BOTH_WRITE
                                              : a_writes         ? CONFLICT_EARLIER_WRITES
                                                                 : CONFLICT_EARLIER_READS};
            vec_push(w->conflicts, c);
        }
    }
}

static bool contains(const decl *const *items, const int count, const decl *d)
{
    for (int i = 0; i < count; i++) {
        if (items[i] == d) return true;
    }
    return false;
}

// Whether the system at place `j` in the tick runs after the one at `i`,
// directly or through others: a bit of `before`, which has a row of `words`
// words per system, a bit per system.
static bool runs_after(const uint64_t *before, const int words, const int j, const int i)
{
    return (before[(size_t)j * (size_t)words + (size_t)(i / 64)] >> (i % 64)) & 1;
}

void analyze_parallelism(program *prog)
{
    const int n = prog->systems.count;
    if (n == 0) return;
    arch_set *sets = arena_alloc(sizeof(arch_set) * (size_t)n);
    // A row per system, with a bit for each system it runs after, directly or
    // through others. A system's row is its place in the tick, `decl.index`.
    const int words = (n + 63) / 64;
    uint64_t *before = arena_alloc(sizeof(uint64_t) * (size_t)n * (size_t)words);
    for (int i = 0; i < n; i++) sets[i] = archetypes_of(prog, prog->systems.items[i]);

    // The systems are in tick order, so everything a system waits for comes first.
    for (int j = 0; j < n; j++) {
        decl *sys = prog->systems.items[j];
        uint64_t *row = before + (size_t)j * (size_t)words;
        sys->stage = 1;
        for (int i = 0; i < j; i++) {
            decl *earlier = prog->systems.items[i];
            system_wait w = {0};
            w.on = earlier;
            find_conflicts(earlier, sys, share_archetypes(&sets[i], &sets[j]), &w);
            w.ordered = contains((const decl *const *)sys->after.items, sys->after.count, earlier);
            if (w.conflicts.count == 0 && !w.ordered) continue;
            vec_push(sys->waits, w);
            // It runs after `earlier`, and after everything `earlier` runs after
            const uint64_t *after = before + (size_t)i * (size_t)words;
            row[i / 64] |= (uint64_t)1 << (i % 64);
            for (int k = 0; k < words; k++) row[k] |= after[k];
            if (earlier->stage + 1 > sys->stage) sys->stage = earlier->stage + 1;
        }
        // A wait goes through another when that other system already waits for
        // it. The waits are in tick order, so only a later one can.
        for (int a = 0; a < sys->waits.count; a++) {
            system_wait *w = &sys->waits.items[a];
            for (int b = a + 1; b < sys->waits.count && !w->through; b++) {
                decl *other = sys->waits.items[b].on;
                if (runs_after(before, words, other->index, w->on->index)) w->through = other;
            }
        }
    }

    for (int j = 0; j < n; j++) {
        decl *sys = prog->systems.items[j];
        for (int k = 0; k < n; k++) {
            if (k != j && !runs_after(before, words, j, k) && !runs_after(before, words, k, j)) {
                vec_push(sys->alongside, prog->systems.items[k]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Text

// A declaration's name as code in `from`'s namespace writes it: short in the
// same namespace, qualified elsewhere. With no `from`, always qualified.
void put_decl_name(sb *out, const decl *d, const char *quote, const decl *from)
{
    const bool same_namespace = from && d->unit && from->unit && str_eq(d->unit->ns, from->unit->ns);
    const str name = same_namespace || d->qualified.len == 0 ? d->name : d->qualified;
    sb_printf(out, "%s" STR_FMT "%s", quote, STR_ARG(name), quote);
}

// Whether `sys` names `earlier` in [After(...)]: otherwise, the order comes from
// [Before(...)] on `earlier`.
static bool has_after(const decl *sys, const decl *earlier)
{
    for (int i = 0; i < sys->attributes.count; i++) {
        const attribute *a = &sys->attributes.items[i];
        if (!str_eq_c(a->name, "After")) continue;
        for (int k = 0; k < a->args.count; k++) {
            if (a->args.items[k].decl == earlier) return true;
        }
    }
    return false;
}

void describe_wait(const decl *sys, const system_wait *w, const char *quote, sb *out)
{
    static const char *const templates[] = {
        [CONFLICT_BOTH_WRITE] = "both write ",
        [CONFLICT_EARLIER_READS] = "it reads ",
        [CONFLICT_EARLIER_WRITES] = "it writes ",
    };
    static const char *const endings[] = {
        [CONFLICT_BOTH_WRITE] = "",
        [CONFLICT_EARLIER_READS] = ", which this writes",
        [CONFLICT_EARLIER_WRITES] = ", which this reads",
    };
    // What the whole world shares, said once each
    static const char *const shared[] = {
        [CONFLICT_TEXT] = "both change text, lists or grids, which the match keeps in one heap",
        [CONFLICT_SPAWN] = "both spawn, and entities get their IDs in order",
        [CONFLICT_TASKS] = "tasks start in order, and spawn and change text as they start",
    };
    bool first_part = true;
    for (int i = 0; i < w->conflicts.count; i++) {
        const conflict_kind kind = w->conflicts.items[i].kind;
        if (kind < CONFLICT_TEXT) continue;
        sb_put(out, first_part ? "" : "; ");
        sb_put(out, shared[kind]);
        first_part = false;
    }
    for (int kind = CONFLICT_BOTH_WRITE; kind <= CONFLICT_EARLIER_WRITES; kind++) {
        int count = 0;
        for (int i = 0; i < w->conflicts.count; i++) count += (int)w->conflicts.items[i].kind == kind;
        if (count == 0) continue;
        sb_put(out, first_part ? "" : "; ");
        sb_put(out, templates[kind]);
        int written = 0;
        for (int i = 0; i < w->conflicts.count; i++) {
            if ((int)w->conflicts.items[i].kind != kind) continue;
            sb_put(out, written == 0 ? "" : written == count - 1 ? " and " : ", ");
            put_decl_name(out, w->conflicts.items[i].data, quote, sys);
            written++;
        }
        sb_put(out, endings[kind]);
        first_part = false;
    }
    if (w->conflicts.count == 0) {
        sb_printf(out, "%s%s", has_after(sys, w->on) ? "this has " : "it has ", quote);
        sb_put(out, has_after(sys, w->on) ? "[After(" : "[Before(");
        put_decl_name(out, has_after(sys, w->on) ? w->on : sys, "", has_after(sys, w->on) ? sys : w->on);
        sb_printf(out, ")]%s", quote);
    }
}

// The components or singletons a system reads (writes = false) or writes.
static void put_access(sb *out, const decl *sys, const bool writes)
{
    int count = 0;
    for (int i = 0; i < sys->params.count; i++) {
        const param *p = &sys->params.items[i];
        const decl *data = accessed(p);
        if (!data || p->mode != (writes ? PARAM_MUT : PARAM_READ)) continue;
        sb_put(out, count++ ? ", " : writes ? "         writes " : "         reads ");
        put_decl_name(out, data, "", NULL);
    }
    if (count) sb_put(out, "\n");
}

void print_schedule(const program *prog, const char *game, sb *out)
{
    int stages = 0;
    int parallel = 0;
    for (int i = 0; i < prog->systems.count; i++) {
        const decl *sys = prog->systems.items[i];
        if (sys->stage > stages) stages = sys->stage;
        if (sys->alongside.count > 0) parallel++;
    }
    sb_printf(out, "%s: %d system%s each tick, %d stage%s deep", game, prog->systems.count,
              prog->systems.count == 1 ? "" : "s", stages, stages == 1 ? "" : "s");
    if (prog->systems.count > 1) {
        if (parallel == 0) sb_put(out, "; none can run at the same time");
        else sb_printf(out, "; %d can run alongside others", parallel);
    }
    sb_put(out, ".\nA system starts once everything it runs after is done.\n\n");

    for (int i = 0; i < prog->systems.count; i++) {
        const decl *sys = prog->systems.items[i];
        sb_printf(out, "stage %-2d ", sys->stage);
        put_decl_name(out, sys, "", NULL);
        sb_put(out, sys->per_entity ? "  (per entity" : "  (once");
        const int loops = parallel_loops(sys->body);
        if (loops) sb_printf(out, ", %d parallel loop%s", loops, loops == 1 ? "" : "s");
        sb_put(out, ")\n");
        put_access(out, sys, false);
        put_access(out, sys, true);
        for (int k = 0; k < sys->waits.count; k++) {
            const system_wait *w = &sys->waits.items[k];
            sb_put(out, "         after ");
            put_decl_name(out, w->on, "", NULL);
            sb_put(out, ": ");
            describe_wait(sys, w, "", out);
            if (w->through) {
                sb_put(out, " (and through ");
                put_decl_name(out, w->through, "", NULL);
                sb_put(out, ")");
            }
            sb_put(out, "\n");
        }
        sb_put(out, "         alongside ");
        for (int k = 0; k < sys->alongside.count; k++) {
            if (k) sb_put(out, ", ");
            put_decl_name(out, sys->alongside.items[k], "", NULL);
        }
        sb_put(out, sys->alongside.count ? "\n" : "nothing\n");
    }

    if (prog->views.count > 0) {
        sb_put(out, "\nViews, every frame in this order (they only read, so nothing waits):\n");
        for (int i = 0; i < prog->views.count; i++) {
            sb_put(out, "         ");
            put_decl_name(out, prog->views.items[i], "", NULL);
            sb_put(out, "\n");
        }
    }

    if (prog->handlers.count > 0) {
        sb_put(out, "\nEvent handlers, at the end of the tick, as their events are sent:\n");
        for (int i = 0; i < prog->events.count; i++) {
            const decl *event = prog->events.items[i];
            if (event->handlers.count == 0) continue;
            sb_put(out, "         ");
            put_decl_name(out, event, "", NULL);
            sb_put(out, ": ");
            for (int k = 0; k < event->handlers.count; k++) {
                if (k) sb_put(out, ", ");
                put_decl_name(out, event->handlers.items[k], "", NULL);
                if (event->handlers.items[k]->is_async) sb_put(out, " (async)");
            }
            sb_put(out, "\n");
        }
    }

    // Tasks go on in a pass of their own, once the tick's changes are in
    bool tasks = false;
    for (int i = 0; i < prog->decls.count && !tasks; i++) {
        const decl *d = prog->decls.items[i];
        tasks = d->is_async && (d->kind == DECL_SYSTEM ? !d->is_local : d->runs_in[0]);
    }
    if (tasks) {
        sb_put(out, "\nTasks go on after the tick's changes, in the order they started; what they change applies after.\n");
    }
}
