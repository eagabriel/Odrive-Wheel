// Parser minimal compatível com a sintaxe do OpenFFBoard Configurator.
//
// Entrada (host -> firmware):  "class.instance.cmd[type][value];"
//   type = '?' get, '=' set, '!' exec, ausente = get
//   value = string após '=' (ex.: "123" ou "1,2,3")
//   vários comandos separados por ';'
//
// Saída (firmware -> host):    "[class.instance.cmdType|reply]"
//   reply = "OK", "ERR", "NOT_FOUND", ou valor formatado

#ifndef CMDPARSER_H_
#define CMDPARSER_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CMD_TYPE_GET  = '?',
    CMD_TYPE_SET  = '=',
    CMD_TYPE_EXEC = '!',
} CmdType;

// Handler de um comando. `reply_buf` tem `reply_size` bytes; preencha com
// string null-terminated. Retorne 0 em sucesso, -1 em erro (reply vira "ERR").
// Pra "NOT_FOUND" a dispatch faz antes (não chega no handler).
typedef int (*CmdHandler)(uint8_t instance, CmdType type,
                           const char *value, char *reply_buf, size_t reply_size);

typedef struct {
    const char *class_name;  // ex.: "odrv"
    const char *cmd_name;    // ex.: "vbus"
    CmdHandler handler;
} CmdEntry;

// Metadados de classe — usados pra responder aos meta-commands
// (id?, name?, help?, cmdinfo?N) que o Configurator dispara em todas as
// classes durante o probe inicial. Sem isso ele marca como "unsupported".
typedef struct {
    const char *class_name;     // "axis", "odrv", etc
    uint16_t    class_id;       // CLSID (ex: 0xA01 pra axis)
    uint8_t     instance;       // normalmente 0
    const char *display_name;   // "Axis 0", "ODrive (M0)", etc
} CmdClassMeta;

// Tabela de comandos registrados (definida em cmd_table.cpp).
extern const CmdEntry      cmdtable[];
extern const size_t        cmdtable_size;
extern const CmdClassMeta  cmdclasses[];
extern const size_t        cmdclasses_size;

// Alimenta bytes recebidos do CDC. O parser bufferiza até encontrar ';' ou
// '\n', então dispara dispatch e coloca reply(s) em out_buf (appende sem
// null-terminator extra). Retorna nº de bytes escritos em out_buf.
size_t cmdparser_feed(const uint8_t *in, size_t in_len,
                       char *out_buf, size_t out_size);

// Zera o estado interno (buffer parcial etc.). Chamar em desconexão.
void cmdparser_reset(void);

#ifdef __cplusplus
}
#endif

#endif
