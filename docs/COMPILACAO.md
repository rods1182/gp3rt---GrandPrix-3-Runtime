# Compilar o snapshot recebido

A preparação deste repositório não recompilou o projeto. Os comandos a seguir refletem os scripts existentes; a reprodução do binário publicado ainda precisa ser validada.

## Windows: cross-compilation em ambiente Linux

`build_win.sh` usa shell POSIX, `clang`, `llvm-rc-18`, `llvm-dlltool-18` e `lld-link-18`, com target `i686-pc-windows-msvc`. Os headers mínimos e definições de importação estão em `src/win/`; o script não usa Windows SDK.

Na raiz do projeto, com as ferramentas instaladas e no PATH:

```sh
sh build_win.sh
```

Saída: `build/win/ddraw.dll`. O script também produz objetos, bibliotecas de importação e mapa. A disponibilidade dos nomes com sufixo `-18` depende da distribuição. Para uso futuro, documentar a distribuição e versões exatas da toolchain e comparar o resultado com a DLL fornecida. Não assumir igualdade de hashes sem uma compilação reproduzível.

## Seleção dos fontes

Este repositório contém as unidades C usadas por `build_win.sh`, os headers compartilhados e o suporte `src/win/` recebidos no snapshot. Seus conteúdos foram preservados. Scripts e unidades exclusivos do Linux, testes do host Linux e ferramentas de projetos separados foram omitidos. Não há dependência desses arquivos no script Windows.

O arquivo `codigo-fonte.tar.gz` do download Windows foi reorganizado com essa mesma seleção de fontes. Não é uma cópia byte a byte do arquivo de fontes misto recebido: seu conteúdo foi reduzido ao escopo Windows sem editar o código.
