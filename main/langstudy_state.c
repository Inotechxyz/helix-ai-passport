// main/langstudy_state.c
#include "langstudy_state.h"

static ls_state_t s_state;

ls_state_t *ls_state(void)
{
    return &s_state;
}
