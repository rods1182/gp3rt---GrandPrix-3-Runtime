# Changelog

## 1.2.1

**English**
- Replay after loading a saved race (GP3 1.13): it only went back to where the race was loaded, because the 1-second snapshots live in memory only. GP3Replay.dll's save also keeps GP3's own snapshots (one every ~32 s) and the whole input buffer; when the replay opens, the part the store lacks is taken from them, and as the replay plays through it the 1-second snapshots are made again (verified tick by tick against the live race). Reported by Stephan Séguin.
- Pressing R right after loading the save (the race standing still, no P first) left the replay stuck rewinding: the race clock was only noted while the race ran. It is now noted every frame.
- New replay keys: Shift+F1 (start of the race) and Shift+F2 (where the race stopped, paused). Neither GP3 nor GPxPatch uses them; both are on the legend.

**Português**
- Replay depois de carregar uma corrida salva (GP3 1.13): voltava só até o ponto do carregamento, porque as fotos de 1 em 1 segundo ficam só na memória. O save do GP3Replay.dll guarda também as fotos do próprio GP3 (uma a cada ~32 s) e o buffer de comandos inteiro; ao abrir o replay, a parte que falta é tirada delas, e as fotos de 1 segundo são refeitas enquanto o replay passa pelo trecho (conferido passo a passo com a corrida). Relatado por Stephan Séguin.
- Apertar R logo depois de carregar o save (corrida parada, sem P antes) deixava o replay preso voltando: o relógio da corrida só era anotado com a corrida andando. Agora é anotado a cada quadro.
- Teclas novas no replay: Shift+F1 (largada) e Shift+F2 (ponto em que a corrida parou, pausado). O GP3 e o GPxPatch não usam essas combinações; as duas estão na legenda.

## 1.2.0

Same version number as GP3RT 1.2.0 for Windows. 1.1.0 was never published: it is included here. / Mesmo número de versão do GP3RT 1.2.0 para Windows. A 1.1.0 não chegou a ser publicada e está incluída aqui.

**English**
- `GP3Replay.dll` (GP3 1.13): whole-race replay, replacing GP3 Video's gp3v.exe; the same file as on Windows, loaded by GP3 Reborn when it is in the game folder. Inspired by Rafa's Gp3Video.
- GP3 2000-style replay keys on GP3 1.13 with an on-screen legend: F1/F2 laps, F3 or P play/pause, F4 slow motion, F5/F8 smooth rewind/fast forward, F6/F7 frame by frame, F9 legend. A packed snapshot per second keeps the replay identical to the race after rewinding.
- GP3 1.13: rain spray and smoke no longer vanish after rewinding the replay.
- GP3 2000: replays up to 2 hours (was 1h30).
- `gp3rt.ini` `[game]`: `gp3replay`, `replay_keys`, `replay_legend`, `replay_fx_fix`, `replay_2h` (same as Windows); new Replay section in the configurator and in the GPxPatch screen.

**Português**
- `GP3Replay.dll` (GP3 1.13): replay da corrida inteira, substituindo o gp3v.exe do GP3 Video; o mesmo arquivo do Windows, carregado pelo GP3 Reborn quando está na pasta do jogo. Inspirado no Gp3Video, do Rafa.
- Teclas do replay do GP3 2000 no GP3 1.13, com legenda na tela: F1/F2 voltas, F3 ou P play/pausa, F4 câmera lenta, F5/F8 voltar/avançar suaves, F6/F7 quadro a quadro, F9 legenda. Uma foto compactada por segundo mantém o replay idêntico à corrida depois de voltar.
- GP3 1.13: o spray da chuva e a fumaça não somem mais depois de voltar o replay.
- GP3 2000: replay de até 2 horas (era 1h30).
- `gp3rt.ini` `[game]`: `gp3replay`, `replay_keys`, `replay_legend`, `replay_fx_fix`, `replay_2h` (iguais ao Windows); seção Replay nova no configurador e na tela do GPxPatch.

## 1.1.0 (not published separately / não publicada à parte)

**English**
- Same engine as GP3RT 1.1.0 for Windows: each frame's drawing goes to the graphics card in one go; textures are converted, reused and mip-mapped with much less work (big help on Intel graphics).
- GP3 2000 no longer closes in long races (bug in GP3 2000's own 3D sound: a lock that doesn't really lock); fixed in memory, always on.
- New `GP3Reborn-Configurator.html`: opens the game folder, reads `gp3rt.ini` and the last logs, shows the graphics card and game found, explains every option.
- Docs: GPxPatch Debug ON (required) and CPU on all cores.
- Thanks to Stephan Séguin ("Seguin"), the first user and the one who spread the word, and to JF ("BeADriver") for the log that revealed the GP3 2000 sound bug.

**Português**
- Mesmo motor do GP3RT 1.1.0 para Windows: o desenho de cada quadro vai para a placa de vídeo de uma vez; texturas convertidas, reaproveitadas e com mipmaps com muito menos trabalho (ajuda muito em placas Intel).
- O GP3 2000 não fecha mais em corridas longas (erro do próprio som 3D do GP3 2000: uma trava que não trava de verdade); corrigido na memória, sempre ligado.
- `GP3Reborn-Configurator.html` novo: abre a pasta do jogo, lê o `gp3rt.ini` e os últimos logs, mostra a placa de vídeo e o jogo encontrados e explica cada opção.
- Documentação: Debug do GPxPatch LIGADO (obrigatório) e CPU em todos os núcleos.
- Agradecimentos ao Stephan Séguin ("Seguin"), primeiro usuário e quem divulgou o projeto, e ao JF ("BeADriver"), pelo log que revelou o erro do som do GP3 2000.

## 1.0.2 (first public release / primeira versão pública)

**English**
- Grand Prix 3 1.13 and Grand Prix 3 2000 running natively on Linux (`gp3` + `gp3-game`).
- Same engine and fixes as GP3RT 1.0.3 for Windows, including the per-frame geometry limit raised from 2,999 to 16,000 edges (no more vanishing car bodies, grey polygons or crashes on very detailed tracks).
- `GPxPatch`: configuration screen and launcher for GPxPatch 4.52 on Linux (GPxPatch itself is not included; get it from https://www.sdidit.nl).
- `GP3Reborn-Configurator.html`: edits `gp3rt.ini` in the browser.
- Bilingual documentation and `gp3rt.ini` comments.

**Português**
- Grand Prix 3 1.13 e Grand Prix 3 2000 nativos no Linux (`gp3` + `gp3-game`).
- Mesmo motor e correções do GP3RT 1.0.3 para Windows, inclusive o limite de geometria por quadro aumentado de 2.999 para 16.000 bordas (sem carros sumindo, polígonos cinza ou fechamento em pistas muito detalhadas).
- `GPxPatch`: tela de configuração e lançador do GPxPatch 4.52 no Linux (o GPxPatch em si não vem junto; baixe em https://www.sdidit.nl).
- `GP3Reborn-Configurator.html`: edita o `gp3rt.ini` no navegador.
- Documentação e comentários do `gp3rt.ini` em inglês e português.
