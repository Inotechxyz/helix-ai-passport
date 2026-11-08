// main/langstudy_session.c
#include "langstudy_session.h"

#include "langstudy_content.h"
#include "langstudy_progress.h"

#include <string.h>

typedef struct {
    uint32_t id;
    uint32_t due;
    uint32_t order;
    uint8_t kind; // 0 due review, 1 unseen, 2 seen but not due yet
    bool excluded;
} candidate_t;

bool ls_session_record_due(const ls_progress_record_t *record,
                           uint32_t session_count)
{
    if (!record) return false;
    uint32_t current = session_count == UINT32_MAX ? UINT32_MAX : session_count + 1;
    return record->due_session <= current;
}

static bool id_in_list(const uint32_t *ids, size_t count, uint32_t id)
{
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == id) return true;
    }
    return false;
}

// Priority order: due reviews first (oldest due first), then unseen words in
// the rotating window, then words that are not due yet. This keeps reviews
// ahead of new material while ensuring a fully studied pack still yields a
// session instead of "NO CARDS".
static bool candidate_before(const candidate_t *a, const candidate_t *b)
{
    if (a->kind != b->kind) return a->kind < b->kind;
    if (a->kind == 1) return a->order < b->order;
    if (a->due != b->due) return a->due < b->due;
    return a->id < b->id;
}

static void sort_candidates(candidate_t *items, size_t count)
{
    // At most LS_WORDS_MAX (100) entries; insertion sort is simple and stable
    // enough for this size.
    for (size_t i = 1; i < count; i++) {
        candidate_t value = items[i];
        size_t pos = i;
        while (pos > 0 && candidate_before(&value, &items[pos - 1])) {
            items[pos] = items[pos - 1];
            pos--;
        }
        items[pos] = value;
    }
}

size_t ls_session_build_ex(const ls_pack_t *pack, const ls_progress_t *progress,
                           const uint32_t *exclude, size_t exclude_len,
                           uint32_t out_ids[LS_SESSION_CARDS])
{
    if (!pack || !out_ids || pack->word_count == 0) return 0;
    if (!exclude) exclude_len = 0;

    size_t word_count = pack->word_count <= LS_WORDS_MAX
                            ? pack->word_count
                            : LS_WORDS_MAX;
    uint32_t session_count = progress ? progress->session_count : 0;
    uint32_t current =
        session_count == UINT32_MAX ? UINT32_MAX : session_count + 1;
    uint32_t offset =
        (uint32_t)(((uint64_t)session_count * LS_SESSION_CARDS) % word_count);

    // Only the UI task (and single-threaded host tests) builds sessions, so a
    // static scratch array keeps ~1.6 KB off the 6 KB application stack.
    static candidate_t candidates[LS_WORDS_MAX];
    size_t candidate_count = 0;

    for (size_t i = 0; i < word_count; i++) {
        const ls_word_t *word = &pack->words[i];
        const ls_progress_record_t *record =
            progress ? ls_progress_find_const(progress, word->id) : NULL;

        candidate_t candidate = {
            .id = word->id,
            .due = 0,
            .order = 0,
            .kind = 1,
            .excluded = id_in_list(exclude, exclude_len, word->id),
        };
        if (record) {
            candidate.due = record->due_session;
            candidate.kind = record->due_session <= current ? 0 : 2;
        } else {
            // Rotate the unseen window by whole sessions so consecutive
            // sessions start at different words.
            candidate.order = (uint32_t)((i + word_count - offset) % word_count);
        }
        candidates[candidate_count++] = candidate;
    }
    sort_candidates(candidates, candidate_count);

    size_t out_count = 0;
    // First pass: never repeat the previous session.
    for (size_t i = 0;
         i < candidate_count && out_count < LS_SESSION_CARDS; i++) {
        if (candidates[i].excluded) continue;
        out_ids[out_count++] = candidates[i].id;
    }
    // Second pass: only when too few other words exist, allow the previous
    // session's words back in so the learner still gets a usable session.
    if (out_count < LS_SESSION_MIN) {
        for (size_t i = 0;
             i < candidate_count && out_count < LS_SESSION_CARDS; i++) {
            if (!candidates[i].excluded) continue;
            if (id_in_list(out_ids, out_count, candidates[i].id)) continue;
            out_ids[out_count++] = candidates[i].id;
        }
    }

    return out_count;
}

size_t ls_session_build(const ls_pack_t *pack, const ls_progress_t *progress,
                        uint32_t out_ids[LS_SESSION_CARDS])
{
    return ls_session_build_ex(pack, progress, NULL, 0, out_ids);
}
