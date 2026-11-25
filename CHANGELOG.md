# Histórico

Resumo do histórico distribuído com o pacote; sem datas de lançamento confirmadas e sem criar mudanças fictícias.

## 1.2.1

- Replay depois de carregar uma corrida salva (GP3 1.13): voltava só até o ponto do carregamento, porque as fotos de 1 em 1 segundo ficam só na memória. O save do GP3Replay.dll guarda também as fotos do próprio GP3 (uma a cada ~32 s) e o buffer de comandos inteiro; ao abrir o replay, a parte que falta é tirada delas, e as fotos de 1 segundo são refeitas enquanto o replay passa pelo trecho (conferido passo a passo com a corrida). Relatado por Stephan Séguin.
- Apertar R logo depois de carregar o save (corrida parada, sem P antes) deixava o replay preso voltando (F5 aceso): o relógio da corrida só era anotado com a corrida andando. Agora é anotado a cada quadro.
- Teclas novas no replay: Shift+F1 (largada) e Shift+F2 (ponto em que a corrida parou, pausado). O GP3 e o GPxPatch não usam essas combinações; as duas estão na legenda.

## 1.2.0

- **GP3Replay.dll** (GP3 1.13): replay da corrida inteira (mais de 2 horas) sem um executável à parte. Substitui o GP3 Video (gp3v.exe): a mesma ampliação do buffer de replay é feita na memória quando o jogo abre, com relocação do código; o Gp3.exe não é alterado e a mesma DLL serve ao Windows e ao Linux. Se o executável já tiver o replay ampliado ou não for o GP3 1.13, nada é alterado (`gp3replay`, padrão 1). Inspirado no Gp3Video, do Rafa.
- Teclas do replay do GP3 2000 no GP3 1.13, com legenda na tela (`replay_keys`, `replay_legend`): F1/F2 voltas, F3 ou P play/pausa, F4 câmera lenta, F5/F8 voltar/avançar, F6/F7 quadro a quadro, F9 legenda. Durante a corrida a DLL guarda uma foto compactada por segundo (quadro-chave a cada 30, diferenças entre eles; ~10 KB por segundo); voltar e avançar restauram essas fotos e o replay continua idêntico à corrida (verificado tick a tick). P e depois R abre o replay já tocando, a partir do início da volta. Pausa, câmera lenta e quadro a quadro sem tremer nem piscar (quadros repetidos não giram os buffers).
- GP3 1.13: o spray da chuva e a fumaça sumiam no replay depois de voltar, porque os geradores de efeito guardam o horário do próximo efeito e ele só anda para a frente. A DLL limita esse horário ao relógio do replay (`replay_fx_fix`, padrão 1).
- GP3 2000: replay de até 2 horas; o limite do controle "Length of replay" passa de 1h30 para 2h00 (`replay_2h`, padrão 1).
- Teclas no Windows lidas também pelo estado do teclado (o jogo usa DirectInput e algumas mensagens se perdiam), só com o jogo em primeiro plano.
- Configurador: nova seção Replay.
- Agradecimentos: Rafa, criador do Gp3Video, pela inspiração.

## 1.1.0

- Desenho bem mais leve para o processador: o desenho de cada quadro é juntado e enviado à placa de vídeo de uma vez, assim como as imagens que o jogo copia para a tela. Num notebook com RTX 3050, em Spa com chuva, o tempo da DLL caiu de cerca de 760 ms para cerca de 80 ms por segundo de jogo.
- PCs sem placa dedicada (Intel): conversão de texturas de 16 bits por tabela, preenchimento de bordas transparentes mais leve, reaproveitamento de texturas que voltam iguais (cache de 64 MB) e mipmaps gerados pelo processador em vez do driver. Engasgos na largada bem menores.
- GP3 2000 fechava em corridas longas (gp3_2000.exe+0x13d49b): erro do próprio som 3D do jogo, cuja "trava" da lista de sons não é atômica e é abandonada após alguns microssegundos. A DLL corrige a trava na memória (`sound_lock_fix`, padrão 1). Diagnóstico a partir do log de JF ("BeADriver"); confirmado numa corrida de 93 minutos.
- Tela cheia: o foco volta ao jogo quando outra janela, escondida atrás dele, rouba o teclado; um clique na tela também devolve o foco.
- Ctrl+F9 salva uma cópia da memória do jogo (diagnóstico).
- Configurador: abre a pasta do jogo, lê o gp3rt.ini e o gp3rt.log, salva direto na pasta (Chrome/Edge), explica cada opção e marca o que difere do padrão.
- Documentação: o Debug do GPxPatch deve ficar ligado com o GP3RT.
- Agradecimentos: Stephan Séguin ("Seguin"), primeiro usuário e divulgador do projeto, e JF ("BeADriver").

## 1.0.3

- Pistas muito detalhadas (ex.: pistas de 1998 do Belini) com a câmera de TV afastada e muitos carros: carros só com as rodas, polígonos cinza e fechamento do jogo. O GP3 reserva espaço para 2.999 bordas de polígono por quadro; ao estourar, deixava um índice antigo no item descartado, que gerava lixo na tela e o fechamento (GP3 2000: gp3.exe+0x69480). A DLL agora amplia o limite para 16.000 bordas e a área de trabalho de ~100 KB para 2 MB, e entrega um índice válido se ainda faltar espaço. GP3 1.13 e GP3 2000; opção `edge_fix` (padrão 1).

## 1.0.2

- GP3 / GP3 2000 fechava sozinho, sem mensagem, ao entrar na pista: duas rotinas de apresentação da imagem da DLL chamavam uma à outra até estourar a pilha. Corrigido.
- O gp3rt.log passa a registrar estouro de pilha, mensagens de erro do jogo e quem encerra o processo.

## 1.0.1

- Proteção da leitura da tabela de strings no GP3 1.13 (`str_guard`), para evitar leitura além do buffer e o fechamento observado com GPxPatch Debug. Patch não aplicado ao GP3 2000.

## 1.0

- Reutilização de memória no caminho de desenho para reduzir sobrecarga em pistas detalhadas.
- Ajustes no envio de imagens para a GPU.
- Reformulação do modo janela.

## Série 0.2.x

O histórico original relata suporte ao GP3 2000, correções gráficas, melhorias de Alt+Enter, entrada/saída, seleção de GPU, widescreen Hor+, spray e diagnóstico. Versões e detalhes completos devem ser cotejados com os snapshots originais antes de criar tags retroativas.

## Preparação para GitHub

Documentação reorganizada em português e inglês, modelos de Issues e guia de publicação. DLL preservada e fontes Windows selecionados sem editar seu conteúdo. O ZIP Windows preparado para publicação usa documentação atualizada e mantém os arquivos funcionais recebidos.
