# Referência do gp3rt.ini — 1.0.1

Valores abaixo copiados do INI fornecido. São os valores **distribuídos**, não uma auditoria de todos os padrões internos quando uma chave é omitida. Feche o jogo antes de editar.

## [video]

| Chave | Valor no pacote | Uso |
| --- | --- | --- |
| `mode` | `fullscreen` | `fullscreen` sem bordas ou `window`. |
| `window_size` | `auto` | `auto` ou tamanho fixo, como `1280x720`. |
| `renderer` | `opengl` | `opengl` ou `software` (fallback mais lento). |
| `gpu` | `auto` | `auto`, `dedicated`, `integrated` ou `system`; confirme a GPU no log. |
| `internal_scale` | `auto` | `auto` (4× dedicada / 2× integrada) ou 1–4; reduza se pesar. |
| `force_filtering` | `1` | 1 força filtragem bilinear. |
| `anisotropy` | `auto` | `auto`, 0, 2, 4, 8 ou 16; depende da GPU. |
| `vsync` | `1` | 1 sincroniza apresentação com o monitor. |
| `aspect` | `keep` | `keep` preserva proporção; `stretch` estica. |
| `spray_opacity` | `60` | Opacidade de spray/fumaça em porcentagem; 100 é o original. |
| `spray_near_fade` | `1` | 1 reduz nuvens de spray muito próximas da câmera. |
| `wide_width` | `-1` | -1 usa desktop; 0 desativa override; ou largura fixa. Configure junto da altura. |
| `wide_height` | `-1` | -1 usa desktop; 0 desativa override; ou altura fixa. Selecione 1280×1024 no jogo. |
| `hor_plus` | `1` | 1 habilita correção Hor+ nas builds reconhecidas. |
| `fov_percent` | `100` | 100 é padrão; maior abre o campo de visão; menor aproxima. |

## [game]

| Chave | Valor no pacote | Uso |
| --- | --- | --- |
| `virtual_cd` | `1` | 1 apresenta ao jogo uma unidade virtual apontando para a pasta local. Não fornece arquivos do jogo nem resolve a proteção SafeDisc. |
| `directplay` | `0` | 0 desativa o caminho DirectPlay na abertura; 1 permite tentativa de rede, sem garantir multiplayer. |
| `str_guard` | `1` | 1 habilita proteção da tabela de strings no GP3 1.13 reconhecido. |

## [advanced]

| Chave | Valor no pacote | Uso |
| --- | --- | --- |
| `lazy_readback` | `1` | 1 habilita leitura diferida usando proteção de páginas. |
| `overlay_locks` | `1` | 1 usa composição pela GPU para caminhos de carros/sombras/spray. |
| `exception_free` | `-1` | -1 automático conforme presença de depurador; 0/1 forçam desligado/ligado. |
| `fast_exit` | `1` | 1 usa encerramento direto após desligar OpenGL. |
| `close_leftover_instances` | `1` | 1 tenta fechar processos antigos do jogo que ficaram presos; pode afetar outra instância em execução. |
| `keep_cpu_affinity` | `0` | 0 permite restaurar todos os núcleos; 1 preserva a afinidade definida externamente. |
| `avoid_exclusive_fullscreen` | `1` | 1 ajusta a janela para evitar comportamento exclusivo do driver. |

## [debug]

| Chave | Valor no pacote | Uso |
| --- | --- | --- |
| `log_level` | `1` | 0 erros; 1 informações; 2 detalhado. |
| `trace_d3d` | `0` | Rastreamento de chamadas gráficas; deixe 0 no uso normal. |
| `frame_dump` | `1` | 1 habilita diagnóstico por Ctrl+F11 / Ctrl+Print Screen. |
