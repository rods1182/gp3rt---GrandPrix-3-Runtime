# Compilar o snapshot recebido

A preparação deste repositório não recompilou o projeto. Os comandos a seguir refletem os scripts existentes; a reprodução do binário publicado ainda precisa ser validada.

## Windows: cross-compilation em ambiente Linux

`build_win.sh` usa shell POSIX, `clang`, `llvm-rc-18`, `llvm-dlltool-18` e `lld-link-18`, com target `i686-pc-windows-msvc`. Os headers mínimos e definições de importação estão em `src/win/`; o script não usa Windows SDK.

Na raiz do projeto, com as ferramentas instaladas e no PATH:

```sh
sh build_win.sh
```

Saída: `build/win/ddraw.dll`. O script também produz objetos, bibliotecas de importação e mapa. A disponibilidade dos nomes com sufixo `-18` depende da distribuição. Para uso futuro, documentar a distribuição e versões exatas da toolchain e comparar o resultado com a DLL fornecida. Não assumir igualdade de hashes sem uma compilação reproduzível.

## Linux

`build_linux.sh` gera `build/linux/gp3`, `gp3-game` e `GPxPatch` (a edição Linux, antes GP3 Reborn). Precisa de clang, de uma musl de 32 bits em `deps/musl32` (com `libclang_rt.builtins-i386.a`) e dos headers do SDL2 (`SDL2_PREFIX`). O passo a passo está em [LINUX-BUILD.txt](../LINUX-BUILD.txt). `build_gp3replay.sh` gera o `GP3Replay.dll`, o mesmo arquivo no Windows e no Linux. `build.sh` gera o executor sem tela usado nos testes (`build/gp3run`).

## Seleção dos fontes

Desde a 1.2.1 este repositório tem o código inteiro: o da `ddraw.dll` do Windows, o do carregador e da tela do GPxPatch do Linux (`host/`) e as ferramentas (`tools/`). Os arquivos de `src/` são os mesmos nos dois sistemas.
