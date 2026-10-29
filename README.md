# GP3RT — Grand Prix 3 Runtime

**Grand Prix 3 e Grand Prix 3 2000 em computadores atuais.**

[English](README.en.md) · [Instalação](docs/INSTALACAO.md) · [Configuração](docs/CONFIGURACAO.md) · [Documentação técnica](docs/ARQUITETURA.md) · [Compilação](docs/COMPILACAO.md)

GP3RT é um projeto independente de Rodrigo Motta (Rods), desenvolvido com assistência do Claude (Anthropic), para manter Grand Prix 3 e Grand Prix 3 2000 jogáveis em computadores atuais.

No Windows, uma `ddraw.dll` específica para esses jogos implementa o caminho gráfico legado usado pelo GP3, com renderização OpenGL. Copie a DLL e o arquivo de configuração para a pasta do jogo; os arquivos do jogo continuam necessários.

## Download

Abra **Releases**, à direita desta página, e baixe o arquivo **GP3RT-1.0.1-Windows.zip** da versão publicada. Os links automáticos **Source code (zip)** e **Source code (tar.gz)** são para desenvolvimento e não incluem a DLL pronta.

## Estado do projeto

| Componente | Estado deste material |
| --- | --- |
| Windows | DLL 1.0.1 fornecida no pacote da comunidade |
| Jogos reconhecidos pelo código | GP3 1.13 e GP3 2000; outras builds podem ter recursos específicos desativados |
| Sistemas alvo | Windows 10 e 11; matriz de testes comunitários ainda em construção |
| Escopo | Somente Windows; o projeto Linux será publicado separadamente quando estiver pronto |
| Verificação desta documentação | Inspeção estática dos arquivos fornecidos; sem execução do jogo ou recompilação |

## Recursos

- Renderizador OpenGL e opção de renderização por software.
- Resolução interna ampliada, filtragem bilinear e anisotrópica configuráveis.
- Widescreen com campo de visão Hor+ nos executáveis reconhecidos.
- Tela cheia sem bordas e modo janela; alternância por Alt+Enter.
- Ajustes de spray da chuva e preferência por GPU dedicada.
- Integração com GPxPatch, capturas PNG, log e diagnóstico de quadros.
- Correção 1.0.1 para leitura de textos do GP3 1.13, incluindo o problema observado com o modo Debug do GPxPatch.

Os recursos foram identificados no código e nos materiais do pacote. Desempenho e compatibilidade dependem de hardware, executável, pista e mods; não há promessa de PO ou FPS específicos.

## Começar a jogar

1. Faça backup da pasta do jogo e dos wrappers que já utiliza.
2. Retire da pasta os wrappers gráficos anteriores que você instalou, especialmente a `ddraw.dll` anterior. Não remova DLLs originais sem identificar sua função.
3. Extraia `ddraw.dll` e `gp3rt.ini` ao lado do executável do jogo. Nunca copie a DLL para System32.
4. Desative modos de compatibilidade do executável e do GPxPatch.
5. No GPxPatch, use **CPU = All / todos os núcleos** (o rótulo pode variar). Comece com **Debug desativado**; pistas que exigem Debug podem usar a correção 1.0.1, conforme [instalação](docs/INSTALACAO.md).
6. Abra o jogo e selecione o modo gráfico acelerado. Para widescreen, selecione **1280×1024**: o GP3RT usa a resolução definida no INI, por padrão a do desktop.

O configurador `GP3RT-Configurador.html`, incluído no download, é opcional. Se o navegador baixar um novo INI, copie-o para a pasta do jogo substituindo o anterior.

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
- GPxPatch, seus autores e a comunidade de Grand Prix 3.

Este é um projeto de fãs sem vínculo oficial com os titulares do jogo. Não inclui o jogo, pistas, texturas ou executáveis comerciais. Nenhuma licença de software foi acrescentada nesta preparação; veja [STATUS-LICENCA.md](STATUS-LICENCA.md).
