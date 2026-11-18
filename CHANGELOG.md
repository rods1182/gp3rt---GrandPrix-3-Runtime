# Histórico

Resumo do histórico distribuído com o pacote; sem datas de lançamento confirmadas e sem criar mudanças fictícias.

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
