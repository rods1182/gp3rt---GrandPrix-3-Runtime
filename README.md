

# GP3RT — Grand Prix 3 Runtime

**Grand Prix 3 e Grand Prix 3 2000 em computadores atuais.**

[English](README.en.md) · [Instalação](docs/INSTALACAO.md) · [Configuração](docs/CONFIGURACAO.md) · [Documentação técnica](docs/ARQUITETURA.md) · [Compilação](docs/COMPILACAO.md)

[Last Release](https://github.com/rods1182/gp3rt---GrandPrix-3-Runtime/releases/latest)

GP3RT é um projeto independente de Rodrigo Motta (Rods), desenvolvido com assistência do Claude (Anthropic), para manter Grand Prix 3 e Grand Prix 3 2000 jogáveis em computadores atuais.

No Windows, uma `ddraw.dll` específica para esses jogos implementa o caminho gráfico legado usado pelo GP3, com renderização OpenGL. Copie a DLL e o arquivo de configuração para a pasta do jogo; os arquivos do jogo continuam necessários.

No Linux, o GP3RT roda o Gp3.exe original sem Wine, com o mesmo código, as mesmas opções do `gp3rt.ini` e uma tela do GPxPatch própria. Essa edição se chamava **GP3 Reborn**; desde a 1.2.1 faz parte deste repositório, e as duas saem juntas, com o mesmo número de versão.

## Download

Abra **Releases**, à direita desta página, e baixe o **GP3RT-1.2.1-Windows.zip** (Windows) ou o **GP3RT-1.2.1-Linux.tar.gz** (Linux 64 bits) da versão publicada. Os links automáticos **Source code (zip)** e **Source code (tar.gz)** são para desenvolvimento e não incluem a DLL pronta.

## Estado do projeto

| Componente | Estado deste material |
| --- | --- |
| Windows | DLL 1.2.1 fornecida no pacote da comunidade |
| Linux | Pacote 1.2.1 (gp3, gp3-game e a tela do GPxPatch), Linux 64 bits com SDL2; GPxPatch 4.52 não incluído |
| Jogos reconhecidos pelo código | GP3 1.13 e GP3 2000; outras builds podem ter recursos específicos desativados |
| Sistemas alvo | Windows 10 e 11; matriz de testes comunitários ainda em construção |
| Escopo | Windows e Linux; a edição Linux era o GP3 Reborn ([repositório antigo, arquivado](https://github.com/rods1182/gp3reborn-linux)) |
| Verificação desta documentação | Inspeção estática dos arquivos fornecidos; sem execução do jogo ou recompilação |

## Recursos

- Renderizador OpenGL e opção de renderização por software.
- Resolução interna ampliada, filtragem bilinear e anisotrópica configuráveis.
- Widescreen com campo de visão Hor+ nos executáveis reconhecidos.
- Tela cheia sem bordas e modo janela; alternância por Alt+Enter.
- Ajustes de spray da chuva e preferência por GPU dedicada.
- Integração com GPxPatch, capturas PNG, log e diagnóstico de quadros.
- Correção 1.0.1 para leitura de textos do GP3 1.13, incluindo o problema observado com o modo Debug do GPxPatch.
- Correção 1.0.2 para o GP3 / GP3 2000 que fechava sozinho ao entrar na pista.
- Correção 1.0.3 para pistas muito detalhadas (ex.: pistas de 1998 do Belini): carros só com as rodas, polígonos cinza e fechamento com a câmera de TV afastada. O limite de geometria por quadro do GP3 passou de 2.999 para 16.000 (`edge_fix=1`).
- 1.1.0: bem mais leve para o processador (o desenho do quadro vai para a placa de uma vez); texturas mais rápidas em PCs sem placa dedicada; correção do GP3 2000 fechando em corridas longas por um erro do próprio som 3D (`sound_lock_fix=1`); foco devolvido ao jogo quando outra janela o rouba escondida atrás dele; configurador que abre a pasta do jogo e lê o gp3rt.ini e o gp3rt.log.
- 1.2.1: o replay volta até a largada também depois de carregar uma corrida salva (usa as fotos que o GP3Replay.dll guarda no save); R logo depois de carregar, sem P, não deixa mais o replay preso voltando; teclas novas Shift+F1 (largada) e Shift+F2 (ponto em que a corrida parou). Relatado por Stephan Séguin.
- 1.2.0: **GP3Replay.dll** (GP3 1.13), o replay da corrida inteira sem executável extra, substituindo o GP3 Video (gp3v.exe), com inspiração no trabalho do Rafa; teclas do replay iguais às do GP3 2000 (F1 a F9, legenda na tela), com voltar e avançar suaves e o replay idêntico à corrida; spray e fumaça que não somem mais depois de voltar o replay; replay de até 2 horas no GP3 2000 (o jogo parava em 1h30). Opções `gp3replay`, `replay_keys`, `replay_legend`, `replay_fx_fix` e `replay_2h`.

Os recursos foram identificados no código e nos materiais do pacote. Desempenho e compatibilidade dependem de hardware, executável, pista e mods; não há promessa de PO ou FPS específicos.

## Começar a jogar

1. Faça backup da pasta do jogo e dos wrappers que já utiliza.
2. Retire da pasta os wrappers gráficos anteriores que você instalou, especialmente a `ddraw.dll` anterior. Não remova DLLs originais sem identificar sua função.
3. Extraia `ddraw.dll` e `gp3rt.ini` ao lado do executável do jogo (no GP3 1.13, também o `GP3Replay.dll`, opcional). Nunca copie a DLL para System32. Com o GP3Replay.dll, abra o jogo direto, sem o gp3v.exe.
4. Desative modos de compatibilidade do executável e do GPxPatch.
5. No GPxPatch, use **CPU = All / todos os núcleos** (o rótulo pode variar). Deixe o **Debug ligado**: com o GP3RT ele é necessário (desligado, o jogo pode fechar ao apertar "Pilotar"/"Drive").
6. Abra o jogo e selecione o modo gráfico acelerado. Para widescreen, selecione **1280×1024**: o GP3RT usa a resolução definida no INI, por padrão a do desktop.

No Linux: extraia o pacote na pasta do jogo e rode `./instalar-atalho.sh`, ou abra o `GPxPatch`. O passo a passo está no [LEIAME do Linux](linux/LEIAME.txt) e a compilação no [LINUX-BUILD.txt](LINUX-BUILD.txt).

O configurador `GP3RT-Configurador.html`, incluído no download, é opcional: clique em "Abrir a pasta do jogo" e ele lê o `gp3rt.ini` e o `gp3rt.log` sozinho. No Chrome e no Edge salva direto na pasta; em outros navegadores baixa o INI para você copiar.

## O que é preservado

A DLL não regrava o executável do jogo no disco. Entretanto, aplica hooks e patches **na memória do processo**, incluindo ajustes de resolução e campo de visão. O projeto procura preservar a experiência original; não é uma reescrita completa do jogo.

## Problemas e colaboração

Abra uma **Issue** com versão do jogo e do GP3RT, Windows, CPU/GPU, driver, GPxPatch, pista, mods, passos para reproduzir e `gp3rt.log`. Copie o log antes de abrir o jogo novamente. Para defeitos gráficos, use Ctrl+F11 e compacte a pasta `screens/frame_NNNNN`. Revise logs e capturas antes de publicar, pois podem conter caminhos pessoais e conteúdo de mods.

Veja [CONTRIBUTING.md](CONTRIBUTING.md), [limitações](docs/LIMITACOES.md) e [histórico](CHANGELOG.md).

## Créditos

- Projeto, direção, testes e validação comunitária: **Rodrigo Motta (Rods)**.
- Desenvolvimento com assistência de **Claude / Anthropic**, conforme os créditos do pacote original.
- Jogo original: **Geoff Crammond / MicroProse**.
- Referência da abordagem de FOV widescreen: **WidePrix3**, de **hatcher the patcher**, conforme comentários do código.
- **Stephan Séguin** ("Seguin" no GrandPrixGames.org): primeiro usuário e primeiro retorno; divulgou o projeto no GrandPrixGames.org, no GP2Forum e no grupo Grand Prix Games do Facebook.
- **JF ("BeADriver")**: o log que revelou o erro do som 3D do GP3 2000 em corridas longas e o teste da correção.
- **Rafa**, criador do Gp3Video, que deu ao GP3 1.13 o replay da corrida inteira: a inspiração do GP3Replay.dll.
- GPxPatch, seus autores e a comunidade de Grand Prix 3.

Este é um projeto de fãs sem vínculo oficial com os titulares do jogo. Não inclui o jogo, pistas, texturas ou executáveis comerciais. Nenhuma licença de software foi acrescentada nesta preparação; veja [STATUS-LICENCA.md](STATUS-LICENCA.md).

# GP3RT — Grand Prix 3 Runtime

**Grand Prix 3 and Grand Prix 3 2000 on modern computers.**

[English guide](docs/USER-GUIDE.en.md) · [Installation](docs/INSTALACAO.md) · [Configuration](docs/CONFIGURACAO.md) · [Technical documentation](docs/ARQUITETURA.md) · [Building from source](docs/COMPILACAO.md)

GP3RT is an independent project by Rodrigo Motta (Rods), developed with assistance from Claude (Anthropic), to keep Grand Prix 3 and Grand Prix 3 2000 playable on modern computers.

On Windows, a game-specific `ddraw.dll` implements the legacy graphics functionality used by GP3, rendering through OpenGL. Copy the DLL and configuration file into the game folder; the original game files are still required.

On Linux, GP3RT runs the original Gp3.exe without Wine, with the same code, the same `gp3rt.ini` options and its own GPxPatch screen. This edition used to be called **GP3 Reborn**; since 1.2.1 it lives in this repository, and both are released together with the same version number.

## Download

Open **Releases** on the right side of this page and download **GP3RT-1.2.1-Windows.zip** (Windows) or **GP3RT-1.2.1-Linux.tar.gz** (64-bit Linux) from the published release. The automatically generated **Source code (zip)** and **Source code (tar.gz)** archives are intended for development and do not include the compiled DLL.

## Project status

| Component | Current status |
| --- | --- |
| Windows | DLL version 1.2.1 supplied in the community package |
| Linux | Package 1.2.1 (gp3, gp3-game and the GPxPatch screen), 64-bit Linux with SDL2; GPxPatch 4.52 not included |
| Game builds recognized by the code | GP3 1.13 and GP3 2000; specific features may be disabled on other builds |
| Target operating systems | Windows 10 and 11; the community testing matrix is still being developed |
| Scope | Windows and Linux; the Linux edition used to be GP3 Reborn ([old repository, archived](https://github.com/rods1182/gp3reborn-linux)) |
| Documentation verification | Static inspection of the supplied files; this documentation review did not include running the game or rebuilding the DLL |

## Features

- OpenGL renderer with an optional software rendering backend.
- Increased internal rendering resolution, with configurable bilinear and anisotropic filtering.
- Widescreen with Hor+ field of view on recognized game builds.
- Borderless fullscreen and windowed modes; switch between them with Alt+Enter.
- Adjustable rain spray and a dedicated GPU preference.
- GPxPatch integration, PNG screenshots, logging, and frame diagnostics.
- Version 1.0.1 fix for GP3 1.13 string-table handling, including the issue observed with GPxPatch Debug mode.
- Version 1.0.2 fix for GP3 / GP3 2000 closing by itself when entering the track.
- Version 1.0.3 fix for very detailed tracks (e.g. Belini's 1998 tracks): cars reduced to wheels, grey polygons and crashes with the TV camera pulled back. GP3's per-frame geometry limit was raised from 2,999 to 16,000 (`edge_fix=1`).
- 1.1.0: much lighter on the processor (each frame's drawing goes to the GPU in one go); faster textures on PCs without a dedicated GPU; fix for GP3 2000 closing in long races because of a bug in its own 3D sound (`sound_lock_fix=1`); focus returned to the game when a hidden window steals it; configurator that opens the game folder and reads gp3rt.ini and gp3rt.log.
- 1.2.1: the replay goes back to the start also after loading a saved race (it uses the snapshots GP3Replay.dll keeps in the save); R right after loading, without P, no longer leaves the replay stuck rewinding; new keys Shift+F1 (start of the race) and Shift+F2 (where the race stopped). Reported by Stephan Séguin.
- 1.2.0: **GP3Replay.dll** (GP3 1.13), a whole-race replay with no extra executable, replacing GP3 Video (gp3v.exe) and inspired by Rafa's work; GP3 2000-style replay keys (F1 to F9, on-screen legend) with smooth rewind and fast forward and a replay identical to the race; rain spray and smoke no longer vanish after rewinding; GP3 2000 replays up to 2 hours (the game stopped at 1h30). Options `gp3replay`, `replay_keys`, `replay_legend`, `replay_fx_fix` and `replay_2h`.

These features were identified in the code and package documentation. Performance and compatibility depend on hardware, executable build, track, and mods; specific PO or FPS values are not guaranteed.

## Getting started

1. Back up your game folder and any wrappers you currently use.
2. Move previously installed graphics wrappers out of the game folder, especially the existing `ddraw.dll`. Do not remove original game DLLs without identifying their purpose.
3. Extract `ddraw.dll` and `gp3rt.ini` alongside the game executable (on GP3 1.13, also the optional `GP3Replay.dll`). Never copy the DLL into System32. With GP3Replay.dll, start the game directly, without gp3v.exe.
4. Disable compatibility modes for the game executable and GPxPatch.
5. In GPxPatch, select **CPU = All / all cores** (the label may vary). Keep **Debug enabled**: with GP3RT it is required (with it off, the game can close when you press "Drive").
6. Launch the game and select hardware-accelerated graphics. For widescreen, select **1280×1024**: GP3RT uses the resolution specified in the INI, which defaults to your desktop resolution.

On Linux: extract the package into the game folder and run `./instalar-atalho.sh`, or open `GPxPatch`. Step by step in the [Linux README](linux/README.txt); building in [LINUX-BUILD.txt](LINUX-BUILD.txt).

The optional `GP3RT-Configurador.html` configuration tool is included in the download. If your browser downloads a new INI file, copy it into the game folder, replacing the previous one.

## What remains unchanged

The DLL does not rewrite the game executable on disk. However, it applies hooks and patches **in the running process’s memory**, including resolution and field-of-view adjustments. The project aims to preserve the original experience; it is not a complete rewrite of the game.

## Issues and contributions

Open an **Issue** with your game and GP3RT versions, Windows version, CPU/GPU, graphics driver, GPxPatch version, track, mods, reproduction steps, and `gp3rt.log`. Copy the log before launching the game again. For graphical defects, press Ctrl+F11 and compress the `screens/frame_NNNNN` folder. Review logs and captures before publishing them, as they may contain personal file paths and mod content.

See [CONTRIBUTING.md](CONTRIBUTING.md), [limitations](docs/LIMITACOES.md), and the [changelog](CHANGELOG.md).

## Credits

- Project, direction, testing, and community validation: **Rodrigo Motta (Rods)**.
- Development assistance from **Claude / Anthropic**, as credited in the original package.
- Original game: **Geoff Crammond / MicroProse**.
- Reference for the widescreen FOV approach: **WidePrix3**, by **hatcher the patcher**, as acknowledged in the code comments.
- **Stephan Séguin** ("Seguin" on GrandPrixGames.org): the first user and the first feedback; he spread the word on GrandPrixGames.org, GP2Forum and the Grand Prix Games group on Facebook.
- **JF ("BeADriver")**: the log that revealed GP3 2000's 3D sound bug in long races, and testing the fix.
- **Rafa**, creator of Gp3Video, who gave GP3 1.13 its whole-race replay: the inspiration for GP3Replay.dll.
- GPxPatch, its authors, and the Grand Prix 3 community.

This is an unofficial fan project with no affiliation with the game’s rights holders. It does not include the game, tracks, textures, or commercial executables. No software license was added during this preparation; see [STATUS-LICENCA.md](STATUS-LICENCA.md).
