# Porte das correções FFB do Forge v1.0.8

Origem: TelksBr/ODrive-Wheel-Forge, commits `c950329e` e `fd664a1`
(https://github.com/TelksBr/ODrive-Wheel-Forge/compare/v1.0.7...v1.0.8).

Integração manual, sem substituir os arquivos pelo fork completo:

- Ganho HID individual aplicado também a Spring, Damper, Friction e Inertia.
- Pause congela duração, atraso de início e fase. Continue retoma o relógio.
- Estado de atuadores habilitados separado de pausa e efeito em execução.
- Set Effect e Continue não anulam Disable Actuators.
- Reset remove efeitos, limpa pausa e habilita atuadores, conforme o Forge.
- Atraso capturado uma vez por Start; atualizar tipo ou atraso não reinicia
  a operação já em andamento.
- Fase periódica infinita acumulada sem salto no rollover do tick HAL.
- Troca de frequência preserva a fração de fase com resolução de 1 ms.
- Primeira leitura de velocidade estabelece a referência da aceleração.

## Adaptações à nossa base

O relógio foi isolado em `advanceEffectClock`, compartilhado pelo cálculo e
pelo GET de estado. Efeitos finitos expiram também com atuadores desabilitados.
Na troca de frequência, avançamos até o instante do report usando a frequência
antiga antes de converter a fase para o período novo.
O acumulador também é usado para periódicos finitos, preservando sua fase
quando a frequência muda sem alterar o relógio de expiração da duração.

Um mutex de tarefa protege cálculo, recepção HID e GET contra atualizações
parciais do novo estado de reprodução. Não desabilita interrupções. Helpers
internos não retomam o mutex, evitando aquisição recursiva. Não é uma revisão
completa de todos os acessos concorrentes existentes via comandos seriais.

Mantidos: cálculo inteiro existente, EQ antes do clip, efeitos locais, batentes,
proteção anti-oscilação e semântica de `axis.invert`/`axis.ffbinvert`. Não foi
adicionada a opção `condinvert`. Não foram importadas otimizações ou outras
funcionalidades dos releases anteriores do Forge. O atraso e a fase exigiram
apenas estado adicional em RAM e unificação do relógio de rampas/envelopes.

Nenhum endereço, formato ou versão de EEPROM mudou. Não é necessário apagar
configurações. O ganho individual dos condicionais passa a ser respeitado:
ganhos abaixo de 255 podem reduzir forças que antes ignoravam esse campo.
Reset deixa os atuadores habilitados e pode mudar a atuação da idle spring,
que depende do FFB estar inativo. Os batentes e os efeitos locais continuam
seguindo suas próprias configurações, inclusive durante Pause/Disable PID.

## Verificação

`Odrive-Wheel/tests/test_forge_v108.py` compila os fontes reais do calculador,
HID, filtros e EQ, além da classe de eixo extraída sem reescrever seus métodos.
Hardware, relógio, flash e RTOS são substituídos por doubles. A versão de teste
do mutex detecta aquisição recursiva, mas não simula preempção do FreeRTOS.

```text
python Odrive-Wheel/tests/test_forge_v108.py --compiler g++
# Alternativa sem compilador nativo: Zig com alvo WASI e Node instalado
python Odrive-Wheel/tests/test_forge_v108.py --zig caminho/zig.exe
# Com bibliotecas WASI já compiladas, evita compilá-las na primeira execução:
python Odrive-Wheel/tests/test_forge_v108.py --zig caminho/zig.exe --wasi-sysroot caminho/wasi-sysroot-24.0
node Odrive-Wheel/tests/test_webhid_reports.cjs
node Odrive-Wheel/tests/test_overlay_errors.cjs
```

Os testes cobrem ganhos condicionais, pausa/retomada, Start durante pausa,
atraso através do rollover, GET de estado, Disable/Enable, Reset, Stop All,
fase e troca de frequência, primeira aceleração, inversões atuais, batente
local e normalização DC do EQ.

Também é necessário compilar para STM32F405 e validar no volante: pausa e
retomada no jogo, ganho dos efeitos condicionais e interação com efeitos locais.
Os testes de software não substituem essa validação física.

Verificação realizada no porte: build ARM GCC 12.2 concluído; teste C++
executado via Zig 0.13 / WASI SDK 24 / Node, com as flags matemáticas do firmware;
testes JavaScript do HTML e do overlay aprovados. Não foi gravado firmware na
placa durante esta integração. O porte foi validado com HTML V0.33;
a preparação do release v1.1.4 atualiza o configurador para V0.34.
