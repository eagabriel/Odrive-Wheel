#include "cmdparser.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

// Buffer de linha incompleta. Comandos típicos < 64 bytes; margem generosa.
#define LINE_BUF_SIZE   256
static char s_line[LINE_BUF_SIZE];
static size_t s_line_len = 0;

// Parseia uma linha isolada (sem ';' nem '\n') e escreve reply em out.
// Retorna nº de bytes escritos em out.
static size_t parse_one(const char *line, char *out, size_t out_size);

void cmdparser_reset(void) {
    s_line_len = 0;
}

size_t cmdparser_feed(const uint8_t *in, size_t in_len,
                       char *out_buf, size_t out_size) {
    size_t out_used = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = (char)in[i];
        if (c == '\r') continue;                    // ignora CR
        if (c == ';' || c == '\n') {
            s_line[s_line_len] = '\0';
            if (s_line_len > 0) {
                size_t n = parse_one(s_line, out_buf + out_used,
                                      out_size > out_used ? out_size - out_used : 0);
                out_used += n;
            }
            s_line_len = 0;
            continue;
        }
        if (s_line_len + 1 < LINE_BUF_SIZE) {
            s_line[s_line_len++] = c;
        } else {
            // overflow: descarta e continua; reply não é emitido
            s_line_len = 0;
        }
    }
    return out_used;
}

// Formata reply no formato do OpenFFBoard StringCommandInterface (ver
// CommandInterface.cpp linhas 118-205). Estrutura:
//   "[class.cmd?|reply]\n"           (GET sem instance)
//   "[class.N.cmd?|reply]\n"         (GET com instance)
//   "[class.cmd=value|reply]\n"      (SET ecoa o valor recebido)
//   "[class.cmd!|reply]\n"           (EXEC)
// Configurator parseia com regex que casa input ↔ reply. Se a estrutura não
// bater (instance presente onde não devia, valor de SET faltando), descarta
// como "unsupported".
static size_t write_reply(char *out, size_t out_size,
                           const char *class_name,
                           bool has_instance, uint8_t instance,
                           const char *cmd, CmdType type,
                           const char *value, const char *reply) {
    if (!out || out_size == 0) return 0;

    // Parte "class[.N].cmd"
    char head[80];
    if (has_instance) {
        snprintf(head, sizeof(head), "%s.%u.%s",
                 class_name, (unsigned)instance, cmd);
    } else {
        snprintf(head, sizeof(head), "%s.%s", class_name, cmd);
    }

    // Sufixo "?", "=value", ou "!"
    char tail[64];
    if (type == CMD_TYPE_SET) {
        snprintf(tail, sizeof(tail), "=%s", value ? value : "");
    } else if (type == CMD_TYPE_EXEC) {
        snprintf(tail, sizeof(tail), "!");
    } else {
        snprintf(tail, sizeof(tail), "?");
    }

    int n = snprintf(out, out_size, "[%s%s|%s]\n", head, tail, reply);
    if (n < 0) return 0;
    if ((size_t)n >= out_size) return out_size; // truncado
    return (size_t)n;
}

// Parse de uma linha: "class.instance.cmdType[value]" ou
// "class.cmd[type][value]". Sem instance especificada, has_instance=false e
// reply omite o número (compat OpenFFBoard CommandInterface, instance 0xFF).
static size_t parse_one(const char *line, char *out, size_t out_size) {
    // Encontra '.' que separa class de resto
    const char *dot1 = strchr(line, '.');
    if (!dot1) return 0; // linha inválida
    size_t class_len = dot1 - line;

    const char *after_class = dot1 + 1;
    uint8_t instance = 0;
    bool has_instance = false;
    const char *cmd_start = after_class;

    // Se "after_class" começar com dígitos seguidos de '.', é a instance
    {
        const char *p = after_class;
        while (*p >= '0' && *p <= '9') p++;
        if (p > after_class && *p == '.') {
            instance = 0;
            for (const char *q = after_class; q < p; q++) {
                instance = instance * 10 + (*q - '0');
            }
            cmd_start = p + 1;
            has_instance = true;
        }
    }

    // Encontra type char (?, =, !) ou fim
    const char *type_p = cmd_start;
    while (*type_p && *type_p != '?' && *type_p != '=' && *type_p != '!') type_p++;

    size_t cmd_len = type_p - cmd_start;
    if (cmd_len == 0) return 0;

    CmdType type = CMD_TYPE_GET;
    const char *value = NULL;
    if (*type_p == '?') {
        type = CMD_TYPE_GET;
        // value após '?' seria endereço (raro) — ignora por enquanto
    } else if (*type_p == '=') {
        type = CMD_TYPE_SET;
        value = type_p + 1;
    } else if (*type_p == '!') {
        type = CMD_TYPE_EXEC;
    } // else '\0' → GET

    // Copia class e cmd pra buffers locais null-terminated
    char class_buf[32];
    char cmd_buf[32];
    if (class_len >= sizeof(class_buf) || cmd_len >= sizeof(cmd_buf)) return 0;
    memcpy(class_buf, line, class_len); class_buf[class_len] = '\0';
    memcpy(cmd_buf, cmd_start, cmd_len); cmd_buf[cmd_len] = '\0';

    // Procura na tabela específica primeiro
    char reply[128] = "NOT_FOUND";
    bool found = false;
    for (size_t i = 0; i < cmdtable_size; i++) {
        if (strcmp(cmdtable[i].class_name, class_buf) == 0 &&
            strcmp(cmdtable[i].cmd_name, cmd_buf) == 0) {
            int rc = cmdtable[i].handler(instance, type, value, reply, sizeof(reply));
            if (rc != 0) strncpy(reply, "ERR", sizeof(reply));
            found = true;
            break;
        }
    }

    // Fallback: meta-commands que toda CommandHandler do OpenFFBoard expõe.
    // Configurator interroga cada classe com id?, name?, help?, cmdinfo?N
    // durante o probe; sem essas respostas marca o device como unsupported.
    if (!found) {
        // Localiza a classe na tabela de metadados
        const CmdClassMeta *meta = NULL;
        for (size_t i = 0; i < cmdclasses_size; i++) {
            if (strcmp(cmdclasses[i].class_name, class_buf) == 0) {
                meta = &cmdclasses[i];
                break;
            }
        }
        if (meta != NULL) {
            if (strcmp(cmd_buf, "id") == 0 && type == CMD_TYPE_GET) {
                snprintf(reply, sizeof(reply), "%u", (unsigned)meta->class_id);
                found = true;
            } else if (strcmp(cmd_buf, "name") == 0 && type == CMD_TYPE_GET) {
                strncpy(reply, meta->display_name, sizeof(reply));
                reply[sizeof(reply)-1] = '\0';
                found = true;
            } else if (strcmp(cmd_buf, "instance") == 0 && type == CMD_TYPE_GET) {
                snprintf(reply, sizeof(reply), "%u", (unsigned)meta->instance);
                found = true;
            } else if (strcmp(cmd_buf, "cmduid") == 0 && type == CMD_TYPE_GET) {
                // Handler ID — usamos o índice da classe na tabela como proxy
                snprintf(reply, sizeof(reply), "%u",
                         (unsigned)(meta - cmdclasses));
                found = true;
            } else if (strcmp(cmd_buf, "help") == 0 && type == CMD_TYPE_GET) {
                // CSV "name,id,flags\n..." dos cmds dessa classe.
                // Flags 0x01 = GET, 0x02 = SET, 0x04 = EXEC. Conservador: GET+SET.
                size_t off = 0;
                int idx = 0;
                for (size_t i = 0; i < cmdtable_size; i++) {
                    if (strcmp(cmdtable[i].class_name, class_buf) != 0) continue;
                    int n = snprintf(reply + off, sizeof(reply) - off,
                                     "%s%s,%d,3",
                                     idx == 0 ? "" : "\n",
                                     cmdtable[i].cmd_name, idx);
                    if (n < 0 || (size_t)n >= sizeof(reply) - off) break;
                    off += (size_t)n;
                    idx++;
                }
                if (off == 0) strncpy(reply, "(empty)", sizeof(reply));
                found = true;
            } else if (strcmp(cmd_buf, "cmdinfo") == 0 && type == CMD_TYPE_GET) {
                // "cmdinfo?N" pede flags do cmd N. Sem GETADR no nosso parser,
                // retornamos flag conservadora (GET+SET = 3). Configurator usa
                // pra decidir se renderiza control de leitura/escrita.
                strncpy(reply, "3", sizeof(reply));
                found = true;
            }
        }
    }

    return write_reply(out, out_size, class_buf, has_instance, instance,
                       cmd_buf, type, value, reply);
}
