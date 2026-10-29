# Instalação e primeiros testes — Windows

## Requisitos e alcance

- Instalação própria do Grand Prix 3 1.13 ou Grand Prix 3 2000.
- Windows 10 ou 11, com suporte a processos x86; a DLL fornecida é **PE32 / Intel 80386**, inclusive para uso em Windows x64.
- Driver gráfico com OpenGL 3.3, conforme requisito declarado pelo projeto.
- Executável compatível com as assinaturas reconhecidas em `src/gamepatch.c`.

A DLL não resolve sozinha a incompatibilidade do executável protegido por SafeDisc descrita no pacote original. Este projeto não fornece executáveis alternativos do jogo. Não use apenas o tamanho do arquivo para identificar uma versão: confirme `game build` no log.

## Passo a passo

1. Feche o jogo e faça uma cópia da pasta inteira.
2. Mova os wrappers gráficos anteriores que você adicionou para uma pasta de backup fora da pasta do jogo. Uma única `ddraw.dll` pode ocupar esse nome. Preserve componentes originais como `eax.dll` do GP3 2000.
3. Copie `ddraw.dll` e `gp3rt.ini` para a mesma pasta do executável que você realmente utiliza.
4. Em Propriedades → Compatibilidade, remova as configurações de compatibilidade do jogo e do GPxPatch. Não habilite privilégios de administrador como requisito padrão.
5. Se usa GPxPatch, configure CPU para todos os núcleos (**All**, **full** ou equivalente na interface). Comece com Debug desligado.
6. Inicie o jogo, use o modo acelerado e teste primeiro uma pista conhecida. Se a calibração marcar falha, feche o jogo, faça backup de `gconfig.txt`, remova-o e calibre novamente.
7. Para widescreen, escolha 1280×1024 no menu. Com `wide_width=-1` e `wide_height=-1`, essa entrada é substituída pela resolução do desktop. Com `hor_plus=1`, a implementação corrige o FOV nas builds reconhecidas.
8. Abra `gp3rt.log` para conferir versão, GPU e reconhecimento do executável.

## GPxPatch Debug na versão 1.0.1

O LEIAME original contém uma orientação antiga para desligar Debug sempre, mas o histórico 1.0.1 e o código adicionam `str_guard=1` para o problema de leitura de strings do GP3 1.13 sob Debug.

Use Debug desligado como teste inicial. Quando uma pista exigir Debug, habilite-o mantendo `[game] str_guard=1` e verifique o resultado. Essa correção é específica do GP3 1.13 e exige assinatura de código reconhecida; não é uma correção geral de todos os travamentos. GP3 2000 não recebe esse patch de strings.

`[advanced] exception_free=-1` escolhe automaticamente um caminho sem certos truques de exceção quando há depurador. `keep_cpu_affinity=0` permite restaurar todos os núcleos, mesmo se o GPxPatch restringir a afinidade.

## Configurador

Abra `GP3RT-Configurador.html` no navegador, carregue o INI e salve. O acesso direto ao arquivo depende do navegador. Quando houver download do INI, substitua manualmente o arquivo na pasta do jogo. Reinicie o jogo para aplicar as mudanças.

## Desinstalação

Feche o jogo e remova apenas a DLL e o INI do GP3RT. Restaure os wrappers anteriores do seu backup, se desejar. Logs e capturas podem ser removidos separadamente. A configuração de preferência da GPU gravada no Windows pode permanecer; altere-a nas configurações gráficas do sistema se necessário.
