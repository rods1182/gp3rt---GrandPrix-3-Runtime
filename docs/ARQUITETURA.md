# Arquitetura — leitura do código fornecido

## Windows

`src/win/ddraw.def` exporta as entradas usadas para carregar a DLL, incluindo DirectDrawCreate e DirectDrawCreateEx. `src/ddraw.c` e `src/d3d.c` implementam as interfaces gráficas legadas usadas pelo jogo. `src/rdr_gl.c` oferece o backend OpenGL; `src/rdr_soft.c`, o backend de software.

`src/win/win_main.c` contém inicialização, leitura do INI e hooks. `src/win/win_plat.c` oferece serviços específicos do Windows; `src/win/win_gpu.c` trata preferências e identificação de GPU. `src/gamepatch.c` reúne endereços e assinaturas para GP3 1.13/GP3 2000, FOV Hor+, tabela de resoluções e proteção de strings do 1.13.

A detecção compara bytes em locais conhecidos do processo; não é uma identificação criptográfica completa do executável. Builds desconhecidas desativam ajustes específicos, sem garantia de funcionamento básico.

O arquivo do jogo no disco não é reescrito por esses patches. O código utiliza proteção de memória e escrita de instruções/dados no processo. Por isso, a descrição correta é **compatibilidade gráfica com ajustes em tempo de execução**, e não ausência absoluta de modificações.

## Ferramentas e diagnóstico

- `tools_ui/configurador.src.html`: fonte do configurador.
- `src/rdr_capture.c` e `src/rdr_framedump.c`: captura e diagnóstico gráfico.

Este documento descreve o snapshot recebido; não certifica segurança, desempenho ou compatibilidade universal.

## Escopo da publicação

Esta pasta contém os fontes usados pelo build Windows e headers compartilhados do snapshot. Arquivos exclusivos do host/loader Linux, seus scripts e ferramentas de outros projetos foram deixados fora. O projeto Linux ainda está em desenvolvimento e terá publicação separada.
