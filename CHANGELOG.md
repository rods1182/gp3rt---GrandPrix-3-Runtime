# Histórico

Resumo do histórico distribuído com o pacote; sem datas de lançamento confirmadas e sem criar mudanças fictícias.

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
