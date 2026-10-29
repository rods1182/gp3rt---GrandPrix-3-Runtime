# Limitações e suporte

- A detecção e os patches específicos foram escritos para builds reconhecidas de GP3 1.13 e GP3 2000. Executáveis diferentes podem perder widescreen/otimizações ou falhar.
- Windows 10/11 são o alvo anunciado. Windows XP/7, ARM e todas as combinações de drivers/mods não estão certificados por esta documentação.
- Não existe benchmark reproduzível anexado que permita prometer uma faixa de PO/FPS. Números dos relatos originais são observações locais.
- O projeto Linux está em desenvolvimento e terá repositório/publicação separados.
- `directplay=1` permite o caminho de rede; não prova funcionamento em LAN ou internet.
- O caminho de software é fallback; pode ser lento em resoluções altas.
- O reconhecimento de GPU e a preferência de alto desempenho dependem do sistema/driver; confira o log e, se necessário, a configuração gráfica do Windows.

## Diagnóstico prático

| Sintoma | Primeiro teste |
| --- | --- |
| Tela preta / erro OpenGL | Conferir instalação e wrappers; atualizar driver oficial; testar `renderer=software` |
| Falha na calibração acelerada | Fechar o jogo, guardar cópia de `gconfig.txt`, removê-lo e repetir |
| Teclado/mouse sem resposta | CPU em todos os núcleos; conferir `keep_cpu_affinity=0` |
| PO alto | Reduzir `internal_scale`; comparar na mesma pista/câmera/clima; testar menos objetos de pista |
| Widescreen ausente | Selecionar 1280×1024; conferir INI e `game build` no log |
| Fechamento com GPxPatch Debug | Usar DLL 1.0.1, `str_guard=1` e conferir reconhecimento de GP3 1.13; comparar sem Debug |
| Configurador parece não salvar | Copiar o INI baixado para a pasta correta e reiniciar o jogo |

Detecções de antivírus não foram investigadas nesta revisão. Não há base aqui para declarar todo alerta um falso positivo ou recomendar exclusões gerais; registre nome/versão da detecção e hash do arquivo para análise.

## Registro de testes sugerido

Use `docs/MATRIZ-TESTES.csv` para registrar testes reais. Registre baseline e GP3RT com a mesma pista, resolução, número de carros, câmera e clima. Preencha somente o que foi observado; deixe como não testado o restante.
