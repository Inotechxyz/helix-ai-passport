// main/langstudy_state.h
// The single application context. The UI task owns pack/progress/session and
// mutates them; network workers only hand back fully built objects through
// langstudy_online, so no lock is needed around this struct.
#pragma once

#include "langstudy_config.h"
#include "langstudy_model.h"
#include "langstudy_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    ls_config_t config;
    ls_pack_t pack;
    ls_progress_t progress;

    ls_store_info_t store;
    uint32_t pack_crc;
    bool storage_ready;

    // Words from recently generated packs, oldest first (space-separated).
    // Persisted in NVS and trimmed by ls_gen_append_history(), so replacing the
    // active pack does not make its vocabulary eligible for reuse again.
    char avoid_history[LS_AVOID_HISTORY_MAX];

    // Last known online status shown as icons on the home screen. The radio is
    // torn down after every online operation, so these are cached results of
    // the most recent Wi-Fi/DeepSeek attempt, not live socket state.
    bool wifi_ok;
    bool deepseek_ok;

    uint32_t session_ids[LS_SESSION_CARDS];
    size_t session_len;
    size_t session_pos;
    uint16_t session_correct;

    // Words from the previous session. The next session skips them so two
    // consecutive sessions never hand the learner the same set. RAM-only: a
    // reboot starts a new rotation.
    uint32_t prev_session_ids[LS_SESSION_CARDS];
    size_t prev_session_len;
} ls_state_t;

ls_state_t *ls_state(void);
