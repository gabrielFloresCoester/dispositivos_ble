# dispositivos_ble

SIMControl: interface web para conectar e configurar dispositivos Coester por BLE. O produto
é o `index.html`: arquivo único, Web Bluetooth, sem dependência externa (nada
carregado de CDN), usado em campo no celular, tablet e notebook. A pasta `ui/`
(PyQt do coleta_ble) foi a referência visual anterior e não faz parte do produto.

## Design

A regra de design é o [docs/DESIGN.md](docs/DESIGN.md) (padrão Coester
Guardian). Antes de criar, alterar ou revisar qualquer tela, componente, cor,
ícone, fonte ou espaçamento, leia-o inteiro; ao terminar, passe a checklist da
seção 15. Ele prevalece sobre `ui/theme.py`.

A pasta `docs/screens/` citada no DESIGN.md ainda não existe. Siga o texto e as
medidas sem esperar pelas imagens.

### Adaptação para HTML puro

O DESIGN.md descreve React, Tailwind e shadcn/ui. Aqui vale a seção 1 dele:
manter tokens, medidas e padrões e traduzir os componentes.

| DESIGN.md | Neste projeto |
|---|---|
| Classes Tailwind (`bg-primary-soft`) | variáveis no `:root` do `index.html` com os nomes da seção 2.4 (`var(--primary-soft)`); o que não está lá sai de um token por `color-mix` e fica marcado como derivado |
| Componentes shadcn/ui | classes CSS próprias com as mesmas medidas (altura, raio, borda, padding) |
| `lucide-react` | `<symbol>` no sprite SVG do `index.html` com o path do Lucide (nome em comentário); nos selos de estado, máscara `--ico-*` |
| Recharts | SVG próprio seguindo a seção 10 |
| `<BrandLogo />` | PNG oficial embutido: `--logo` (azul, fundo claro) e `--logo-white` (fundo `--primary`). Originais em `docs/brand/` |
| Fonte Inter | `@font-face` com woff2 embutido: Inter 4.001 (OFL), pesos 400 a 700, tamanho óptico de texto, só latino. Gerado do `InterVariable.ttf` com fontTools (`varLib.instancer` + `subset`) |

### Decisões tomadas (2026-10-06)

- Moldura: de 768 px para cima, fundo `--sidebar` em volta e o conteúdo num
  painel `--background` arredondado (inset). A coluna de dispositivos fica
  clara, dentro do painel, para os pontos de estado continuarem legíveis. O
  PanelLeft no topo recolhe a coluna. No celular não há moldura.
- Desconectado: a tela de boas-vindas é o login do DESIGN.md (9.1), com fundo
  `--primary`, logo branco e cartão branco. O topo some nesse estado.
- Falha e alarme seguem o DESIGN.md à risca: fundo `--danger-soft`, borda
  `--border`, ícone CircleX em `--danger` e texto em `--anomaly`. Sem fundo
  vermelho. `--danger` só em ícone e ponto (3,8:1 no branco).
- Log da aba Terminal continua escuro, montado só com tokens (`--console-*`).
- Toque (`pointer: coarse`): controles de 44 px, acima dos 40 px do DESIGN.md.
- Título de página: 30 px; abaixo de 600 px, 24 px (com 30 px o comando saía
  da tela no celular).

### Pendências

- Toasts não estão no DESIGN.md (seção 14): ficaram com tokens e borda
  colorida à esquerda, `[design-pendente]`.
- O nº de alarmes sobre o ícone do Atuador conectado, na coluna recolhida, e o nº de
  alarmes ativos na aba Alarmes usam fundo `--destructive` cheio.
- Partes da seção 14 (lacunas): implementar com os tokens existentes e avisar
  que ficaram `[design-pendente]`. Decisão nova se pergunta, não se decide.

### Decisões do produto que o DESIGN.md não cobre

- Não pode quebrar em nenhuma largura: validar de 320 a 2560 px.
- Nomes nos textos exibidos ao cliente (2026-10-08): o equipamento é
  "Atuador" (nunca "Gateway" nem "SIM Connect") e a interface é
  "SIMControl" (nunca "interface", "painel" ou "página"). Quando o texto
  fala também de um atuador da lista, o equipamento é "o Atuador
  conectado". Comentários e nomes de código não mudam.
- Conexão (2026-10-09): do clique em Conectar até a tela do Atuador abrir,
  o botão fica em cor cheia com o LoaderCircle girando (DESIGN.md 8.1) e
  diz o passo: "Conectando…", "Lendo parâmetros…" e, depois da senha,
  "Carregando dados…". A tela do Atuador só abre depois da primeira
  leitura de posição e torque (no máximo 4 s).
- Painel do Atuador conectado (2026-10-07): a página não rola em nenhum momento.
  - Aba Operação, num bloco só: Posição (centralizada, cresce com a tela)
    e, ao lado, nesta ordem (2026-10-08): Abrir/Parar/Fechar, Modo de
    operação e Torque. No celular o torque vai numa faixa no pé do bloco.
    O valor AD da posição não aparece. Nas pontas do curso (2026-10-09),
    até 0,3 % o número dá lugar a "Fechado" e a partir de 99,7 % a
    "Aberto", só na tela (o valor lido não muda). O aviso sob o modo só aparece fora do
    modo SIMControl. Os modos (2026-10-09) são "SIMControl", "Rede" e
    "Desligado" no seletor e no selo; "Via SIMControl" e "Via Interface de
    Rede" só na dica do botão e no aviso de troca (por extenso não cabiam). Abrir/Parar/Fechar visíveis sem rolar em notebook (janela de
    ~1097×760) e no celular.
  - Aba Identificação (a última): Tag, Modelo e N/S e o nome Bluetooth
    anunciado. Saíram de Gerais, Fábrica e Parâmetros. Modelo e N/S
    continuam só leitura até a senha de fábrica.
  - Em tela grande, textos e botões crescem na proporção do mostrador
    (escala `--u` no palco; topo, abas e nome com `--g` a partir de
    1440 px). As
    medidas do DESIGN.md são o tamanho mínimo.
  - Alarmes, Entradas e saídas e Eventos têm aba própria, entre Operação e
    Parâmetros. A aba Alarmes mostra quantos estão ativos. Em cima do card,
    as seis abas ficam numa linha com o nome inteiro a partir de 740 px
    úteis, numa linha com "E/S" de 640 a 739 px e numa grade 3 × 2 abaixo
    de 640 px (a linha mede 723 px com o nome inteiro e 614 com "E/S").
  - Aba Alarmes (2026-10-08): só os alarmes ativos; sem nenhum, "Nenhum
    alarme ativo" com CircleCheck.
  - Atuador da lista (visto pelo Atuador conectado), 2026-10-08: abas
    Diagnóstico, Alarmes e Terminal. A aba Alarmes mostra só os ativos,
    sem rolagem própria, com o nº na aba; o Diagnóstico fica com
    Posição, Torque e Dados gerais.
  - Aba Entradas e saídas (2026-10-09): oito canais, A a D analógicos e
    E a H digitais, ocupam a aba toda. 4 × 2 (analógicos em cima) quando
    a área é larga e baixa, como no notebook; 2 × 4 quando é estreita ou
    alta; uma coluna só no celular de 320 px. Cada quadro empilha nome,
    valor, tipo e função; o valor cresce com a largura do quadro (até
    44 px). Onde não cabem (celular), os quadros rolam por dentro.
  - Parâmetros (2026-10-08): o grupo Gerais (LEDs, inibe comando local,
    idioma) não aparece; a aba ficou só "Alarme". Em E/S de campo
    (2026-10-09), A a D só aceitam Desativado, Entrada analógica e Saída
    analógica; E a H, Desativado, Entrada digital e Saída digital, numa
    seção para cada grupo.
  - Aba Eventos (2026-10-08): o registro de eventos gravado no Atuador
    (flash, branch `feature-regEven`, `docs/REGISTRO_EVENTOS.md`), no
    lugar dos "eventos desta conexão". Lê ao abrir a aba, 100 por vez,
    do mais novo para o mais antigo; tabela Tempo / Evento / Nº, com o
    tempo desde a energização enquanto não houver relógio.
  - Em Parâmetros, Amplia tempo tem aba própria (2026-10-08): junto de ESD
    e PST, a aba rolava.
  - Abas do Atuador conectado em 16 px (o DESIGN.md dá 14; 14 só abaixo de 340 px
    úteis). A partir de 1024 px, com o painel do Atuador conectado aberto, ficam no
    topo da página, ao lado de "SIMControl", separadas por um traço
    vertical (`posicionarAbas` move o nó). Até 1319 px o selo de conexão
    vira só o ponto e o Desconectar só o ícone; até 1151 px "Entradas e
    saídas" vira "E/S". Abaixo de 1024 px (tablet em pé, celular) ficam em
    cima do card, com o Voltar no celular.
  - Nome do atuador, selo de modo e "Atualizado às" ficam no canto
    superior direito do card da Operação (no celular, em cima). Tag,
    Modelo e N/S só na aba Identificação.
  - Onde não cabe (celular com menos de ~390×844, janela muito baixa), a aba
    rola por dentro; a página, nunca.
- A coluna lateral recolhida mantém à vista o Atuador conectado e quantos
  atuadores há.
- O operador precisa bater o olho e saber: arco da posição em azul; torque
  normal só com contorno verde leve (`--success-border`).
