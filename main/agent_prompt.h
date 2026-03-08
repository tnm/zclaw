#ifndef AGENT_PROMPT_H
#define AGENT_PROMPT_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    AGENT_PERSONA_NEUTRAL = 0,
    AGENT_PERSONA_FRIENDLY,
    AGENT_PERSONA_TECHNICAL,
    AGENT_PERSONA_WITTY,
} agent_persona_t;

const char *agent_persona_name(agent_persona_t persona);
bool agent_parse_persona_name(const char *name, agent_persona_t *out);
const char *agent_build_system_prompt(agent_persona_t persona, char *buf, size_t buf_len);

#endif // AGENT_PROMPT_H
