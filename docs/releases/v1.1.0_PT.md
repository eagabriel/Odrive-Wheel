**Full Changelog**: https://github.com/eagabriel/Odrive-Wheel/compare/v1.0.0...v1.1.0

🎉 Odrive-Wheel v1.1.0 — O que há de novo

Release focada em fazer o force feedback realmente funcionar nos jogos onde ele falhava em silêncio, além de perfis de ajuste que você troca conforme o carro.

🎮 Force feedback corrigido em vários jogos
Nove defeitos distintos na camada USB HID PID foram encontrados e corrigidos. O pior deles: num volante de eixo único, qualquer efeito enviado com direção de 0° ou 180° produzia **torque zero** — a força inteira caía num eixo Y que não existe fisicamente. Essa é a direção padrão que muitos jogos usam, e é por isso que o volante podia ficar completamente solto sem motivo aparente.

Também corrigidos: a Forza desligava o FFB sozinha porque o volante respondia "sem energia" quando perguntado sobre seu estado. Mola, damper e atrito ficavam mudos em Assetto Corsa, AMS2 e rFactor 2 porque esses jogos enviam "sem limite de saturação" e o firmware lia como "corte tudo em zero". Trocar de carro ou pista podia travar a placa por completo. Efeitos voltavam sozinhos depois de uma batida ou de um reset. Jogos da Codemasters (DiRT, F1, GRID) perdiam mola e damper por inteiro. Sessões longas acumulavam jitter na vibração e vazavam slots de efeito aos poucos, até o volante começar a recusar efeitos novos.

**Para quem é:** todo mundo. Se você já sentiu "o FFB simplesmente morreu" ou "esse jogo está estranho e eu não sei por quê", essa é a release.

🗂 Perfis de ajuste
Nova aba **Perfis**. Salve sua configuração com um nome — GT3 seco, Rally chuva, Kart — e alterne entre eles com um clique.

Um perfil guarda como o volante *se sente*: as três abas de FFB, as bandas do EQ e três parâmetros de ajuste. Ele deixa de fora, de propósito, a identidade do motor e do encoder, então carregar um perfil nunca pode quebrar sua calibração. Os perfis são arquivos comuns numa pasta que você escolhe, e a pasta é lembrada entre sessões — dá para fazer backup, sincronizar pelo Drive ou OneDrive e mandar para um amigo.

Aplicar um perfil escreve na placa mas não grava na memória permanente: teste na pista e, se não gostar, basta reiniciar para voltar ao que você tinha. Clique em Salvar só quando estiver satisfeito.

**Para quem é:** quem pilota mais de uma categoria de carro, ou divide o rig com outras pessoas.

🎚 EQ de 3 bandas
Novo EQ na aba FFB Filters com três bandas — **WEIGHT** (5 Hz), **CHASSIS** (12 Hz) e **ROAD** (25 Hz), ±12 dB cada — que remodelam as forças vindas do jogo antes de chegarem ao motor. Aumente ROAD para mais zebra e textura de pista, aumente CHASSIS para mais rolagem de carroceria e movimento de suspensão.

O ponto importante: o peso na curva permanece exatamente o mesmo, não importa como você ajuste as bandas. Você calibra o torque máximo uma vez e nunca mais mexe — o EQ muda só a textura, jamais o quanto o volante é pesado.

**Para quem é:** quem quer mais detalhe sem aumentar o ganho geral, ou acha que uma faixa de frequência está dominando demais.

🧲 Encoder MT6835 pronto para uso real
O MagnTek MT6835 de 21 bits (2.097.152 contagens por volta) agora é utilizável em produção, não mais só em caráter experimental.

Ler esse encoder sufocava o USB: três quadros SPI mais um checksum dentro da interrupção de controle de 8 kHz, com um reset completo do periférico a cada alternância entre encoder e driver de gate. A taxa de entrada do force feedback despencava de cerca de 670 Hz para algo entre 2 e 45 Hz. Essa leitura agora roda em uma thread própria, e a alternância só reescreve o que de fato muda.

A ferramenta de configuração ganhou um painel MT6835 mostrando estado da comunicação, avisos de campo magnético fraco e estado da calibração, além de botões para definir o zero mecânico e gravá-lo permanentemente no chip do encoder — com a espera obrigatória de 6 segundos já tratada para você.

**Para quem é:** donos de ODESC V4.2 usando a porta SPI exposta, ou quem quer resolução maior que a do AS5047.

🔇 Motor mais silencioso, efeitos mais nítidos
Damper, atrito e inércia obtinham velocidade derivando a posição bruta do encoder duas vezes, o que amplificava enormemente o ruído do sensor e obrigava a baixar a banda da malha de corrente só para esconder o chiado resultante.

Agora eles leem a velocidade direto do estimador do próprio encoder. O motor ficou mais silencioso, e `encoder.config.bandwidth` passa a ser o controle real de quão nítidos ou suaves esses efeitos ficam — em encoders absolutos dá para levar a 1000–2000 Hz.

↔️ Inversão de eixo e de FFB separadas
`axis.invert` e `axis.ffbinvert` agora são independentes. Um inverte para que lado o jogo vê o volante girando, o outro inverte para que lado as forças empurram. Antes eles estavam amarrados, então corrigir um quebrava o outro.

⚠️ Vindo de uma versão anterior com `axis.invert` ligado? Ligue também o `axis.ffbinvert` para manter o comportamento de antes.

🌡 Temperatura do motor e dos MOSFETs
`sys.temp` e `sys.motortemp` informam os termistores do inversor e do motor em °C, para que ferramentas externas e dashboards possam acompanhar o thermal derating.

⚡ Motores de alta impedância aceitos
A medição de indutância de fase rejeitava qualquer valor acima de 4 mH, reprovando a calibração de motores de alta impedância perfeitamente bons. O limite agora é 25 mH.

---

**Gravação:** baixe o `odrive-wheel-v1.1.0.hex` abaixo e grave via DFU, ou use o botão 📡 Fetch latest from GitHub na aba DFU.

**Ferramenta de configuração:** <https://eagabriel.github.io/Odrive-Wheel/> (Chrome/Edge)

**Dúvidas e relatos de bug:** [participe da discussão no Discord](https://discord.com/channels/704355326291607614/1499185654033158305)

Obrigado ao [@aksc857-stack](https://github.com/aksc857-stack) pelo trabalho no encoder MT6835 — driver, leitura em thread, guarda atômica no arbiter SPI e acesso aos registradores do chip — além da velocidade e aceleração pela PLL do encoder e da separação das inversões; e ao [@TelksBr](https://github.com/TelksBr) ([ODrive-Wheel-Forge](https://github.com/TelksBr/ODrive-Wheel-Forge)) pelo conjunto de correções de HID PID e pela telemetria térmica.
