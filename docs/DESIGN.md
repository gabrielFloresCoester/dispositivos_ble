# DESIGN.md — Referência de UI/UX (padrão Coester)

> **Para agentes do Claude Code:** este arquivo é a fonte de verdade visual e de UX para criar e ajustar telas deste produto. Leia-o inteiro antes de criar ou alterar qualquer componente de interface. As telas de referência estão em `./screens/` — abra a imagem correspondente sempre que for reproduzir um padrão.
>
> **Regras inegociáveis**
> 1. **Cores:** use *exclusivamente* os tokens da seção 2. Nunca invente hex, nunca use a paleta default do Tailwind diretamente (`bg-blue-600`, `text-gray-500` etc.) — sempre os tokens semânticos.
> 2. **Logo:** o logo oficial é o **Coester** (versão branca sobre o azul primário). Não redesenhe, não recolora, não distorça. Use o arquivo oficial do repositório (seção 7).
> 3. **Consistência > criatividade:** antes de criar um componente novo, procure um equivalente neste documento e reutilize-o.
> 4. Valores marcados com **≈** foram medidos a partir de screenshots (precisão de ±2px). Cores **não** são aproximadas: foram extraídas pixel a pixel e são exatas.

---

## Sumário

1. Stack e convenções
2. Tokens de cor
3. Tipografia
4. Espaçamento, raios, bordas e sombras
5. Iconografia
6. Layout e estrutura de páginas (app shell)
7. Logo e marca
8. Componentes
9. Padrões de tela (templates)
10. Gráficos
11. Estados e semântica de status
12. Conteúdo e microcopy (pt-BR)
13. Acessibilidade e responsividade
14. Lacunas do design (o que não está especificado)
15. Checklist de revisão para PRs de UI

---

## 1. Stack e convenções

O design foi construído sobre a linguagem visual do **shadcn/ui** (paleta Slate + cor primária customizada). A implementação de referência é:

| Camada | Escolha |
|---|---|
| Framework | React (Next.js ou Vite — seguir o que o projeto já usa) |
| Estilo | Tailwind CSS com tokens via CSS variables |
| Componentes base | shadcn/ui (`Button`, `Input`, `Label`, `Card`, `Select`, `Tabs`, `Table`, `Badge`, `Dialog`, `DropdownMenu`, `Avatar`, `Sidebar`, `Pagination`, `Separator`) |
| Ícones | `lucide-react` |
| Gráficos | Recharts (via `components/ui/chart` do shadcn, se existir) |
| Fonte | Inter (ver seção 3) |

Se o projeto usar outra stack, mantenha **os tokens, medidas e padrões** deste documento e traduza os nomes de componentes.

---

## 2. Tokens de cor

### 2.1 Paleta base (valores exatos)

| Token | Hex | Uso observado |
|---|---|---|
| `--primary` | `#1A3C85` | Cor da marca. Fundo do login, sidebar/moldura do app, botões primários, ícones de destaque, link "Previous", borda tracejada do upload |
| `--primary-foreground` | `#FAFAFA` | Texto/ícone sobre `--primary` (no login aparece como `#FFFFFF`; ambos aceitos) |
| `--primary-strong` | `#122B5E` | Texto das abas (Tabs) sobre fundo `--primary-soft` |
| `--primary-muted-fg` | `#48639D` | (= primary a 80% sobre branco) Rótulos de métricas ("Torque", "Posição"…), texto "Última atualização" |
| `--primary-soft` | `#E8ECF3` | (= primary a 10% sobre branco) Fundo do container de Tabs, item ativo do menu lateral de configurações, caixas de ícone neutras, avatar de iniciais na tabela, área de upload |
| `--background` | `#F8FAFC` | Fundo das páginas internas (slate-50); fundo do corpo do modal; fundo do input de busca; botão outline |
| `--card` | `#FFFFFF` | Cards, inputs, selects, card do login |
| `--foreground` | `#0F172A` | Texto principal, títulos, números de KPI, ícones neutros (slate-900) |
| `--muted` | `#F1F5F9` | Cabeçalho de tabela, cabeçalho do modal (slate-100) |
| `--muted-foreground` | `#64748B` | Texto secundário, descrições, placeholders, ticks de eixo, ícones secundários (slate-500) |
| `--border` | `#E2E8F0` | Bordas de cards de conteúdo, tabela, divisores de linha, modal, badges outline, botão outline (slate-200) |
| `--input` | `#CBD5E1` | Bordas de inputs/selects, cards de métrica do dashboard, divisor do header da página, divisor do card de gráfico, avatar do menu do usuário (slate-300) |

### 2.2 Cores semânticas (status)

| Token | Hex | Uso |
|---|---|---|
| `--success` | `#047857` | Ícone/texto de sucesso, "Ativo", "Normal" (emerald-700) |
| `--success-soft` | `#ECFDF5` | Fundo de badge/caixa de ícone de sucesso (emerald-50) |
| `--success-border` | `#A7F3D0` | Borda do badge "Ativo" (emerald-200) |
| `--warning` | `#B45309` | Ícone/texto de alerta, "Suspenso" (amber-700) |
| `--warning-soft` | `#FFFBEB` | Fundo de badge/caixa de ícone de alerta (amber-50) |
| `--warning-border` | `#FDE68A` | Borda do badge "Suspenso" (amber-200) |
| `--danger` | `#EF4444` | Ícone do KPI "Atuadores com problema" (red-500) |
| `--danger-soft` | `#FAFAFA` | Fundo da caixa de ícone do KPI de problema (valor exatamente como está no design) |
| `--anomaly` | `#D12E2E` | Ícone do badge "Anomalia" e série "Descida" do gráfico |

### 2.3 Cores de gráfico

| Token | Hex | Uso |
|---|---|---|
| `--chart-1` | `#5583E8` | Série "Subida" (linha e marcador de legenda) |
| `--chart-2` | `#D12E2E` | Série "Descida" (linha e marcador de legenda) |
| `--chart-grid` | `#F0F3F7` | Linhas de grade horizontais |
| Pontos | mesma cor da série com **opacidade 0.8** (resulta em `#779CED` e `#DA5858` sobre branco) |

### 2.4 Implementação (CSS variables)

```css
/* globals.css */
@layer base {
  :root {
    --primary: #1A3C85;
    --primary-foreground: #FAFAFA;
    --primary-strong: #122B5E;
    --primary-muted-fg: #48639D;
    --primary-soft: #E8ECF3;

    --background: #F8FAFC;
    --foreground: #0F172A;
    --card: #FFFFFF;
    --card-foreground: #0F172A;
    --popover: #FFFFFF;
    --popover-foreground: #0F172A;
    --muted: #F1F5F9;
    --muted-foreground: #64748B;
    --secondary: #F1F5F9;
    --secondary-foreground: #0F172A;
    --accent: #E8ECF3;
    --accent-foreground: #0F172A;
    --border: #E2E8F0;
    --input: #CBD5E1;
    --ring: #1A3C85;

    --success: #047857;
    --success-soft: #ECFDF5;
    --success-border: #A7F3D0;
    --warning: #B45309;
    --warning-soft: #FFFBEB;
    --warning-border: #FDE68A;
    --danger: #EF4444;
    --danger-soft: #FAFAFA;
    --anomaly: #D12E2E;
    --destructive: #D12E2E;

    --chart-1: #5583E8;
    --chart-2: #D12E2E;
    --chart-grid: #F0F3F7;

    --sidebar: #1A3C85;
    --sidebar-foreground: #FAFAFA;

    --radius: 0.75rem; /* 12px — cards */
  }
}
```

Tailwind v4 (`@theme inline`) ou v3 (`theme.extend.colors`): mapear cada variável para uma classe utilitária com o mesmo nome (`bg-primary`, `bg-primary-soft`, `text-primary-strong`, `text-primary-muted-fg`, `border-input`, `bg-success-soft`, `text-warning`, etc.).

```ts
// tailwind.config.ts (v3) — trecho
colors: {
  primary: { DEFAULT: 'var(--primary)', foreground: 'var(--primary-foreground)',
             strong: 'var(--primary-strong)', 'muted-fg': 'var(--primary-muted-fg)', soft: 'var(--primary-soft)' },
  success: { DEFAULT: 'var(--success)', soft: 'var(--success-soft)', border: 'var(--success-border)' },
  warning: { DEFAULT: 'var(--warning)', soft: 'var(--warning-soft)', border: 'var(--warning-border)' },
  danger:  { DEFAULT: 'var(--danger)',  soft: 'var(--danger-soft)' },
  anomaly: 'var(--anomaly)',
  // + background, foreground, card, muted, border, input, ring, chart-*
}
```

### 2.5 Regras de aplicação de cor

- **Duas tonalidades de borda, com papéis distintos:** `--input` (`#CBD5E1`) para campos de formulário e cards de métrica do dashboard; `--border` (`#E2E8F0`) para cards de conteúdo, tabelas, modais, badges e botões outline. Não misture.
- **Azul primário é para ação e marca.** Ícones de métricas, botão primário, link ativo de paginação. Não use o primário em texto corrido.
- **Status sempre em trio** (fundo soft + borda + texto/ícone forte), nunca só a cor do texto.
- O fundo `#F8FAFC` é da *página*; o branco é das *superfícies* (cards, inputs). Essa diferença é a principal forma de hierarquia — evite sombras pesadas.

---

## 3. Tipografia

**Família:** Inter (identificada visualmente nas telas — confirmar no Figma). Fallback: `ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif`.

```ts
// Next.js
import { Inter } from 'next/font/google'
const inter = Inter({ subsets: ['latin'], variable: '--font-sans' })
```

### Escala (valores lógicos ≈)

| Papel | Tamanho / peso | Cor | Exemplo |
|---|---|---|---|
| Título de página (H1) | 30px (`text-3xl`) / bold, `tracking-tight` | foreground | "Tenants" |
| Número de KPI | 30px (`text-3xl`) / bold | foreground | "4", "3" |
| Título de seção de configurações | 20–24px (`text-xl`/`text-2xl`) / medium | foreground | "Alterar Senha" |
| Título de card / modal | 16–18px (`text-base`/`text-lg`) / semibold | foreground | "Todos os tenants", "Torque em relação a posição", "Tenant criado" |
| Título do card de login | 16px / semibold | foreground | "Entre na sua conta" |
| Título no header da aplicação | 16px / regular | foreground | "Configurações do Usuário", "Tenants" |
| Valor de métrica | 16px / medium | foreground | "68.7 Nm" |
| Corpo / inputs / botões / abas | 14px (`text-sm`) / regular (botões e abas: medium) | — | — |
| Label de formulário | 14px / medium | foreground | "Senha atual" |
| Descrição / subtítulo | 14px / regular | muted-foreground | "Realize a alteração da sua senha" |
| Rótulo de KPI | 14px / regular | muted-foreground | "Total de atuadores" |
| Rótulo de métrica | 14px / regular | primary-muted-fg | "Torque", "Temperatura" |
| Cabeçalho de tabela | 13–14px / semibold, **UPPERCASE** | foreground | "STATUS", "ACESSO ATÉ" |
| Texto auxiliar / hint | 12–14px / regular | muted-foreground | "PNG ou JPG - max. 2MB", "1-4 de 4" |
| Ticks de eixo | 12px / regular | muted-foreground | "20", "40" |

Regras:
- Sentence case em títulos, botões e labels (ex.: "Salvar alterações", não "Salvar Alterações"). **Exceção:** cabeçalhos de tabela em caixa alta.
- Números de KPI e de métricas usam `tabular-nums` para não "pular" ao atualizar.

---

## 4. Espaçamento, raios, bordas e sombras

Base de 4px (escala Tailwind). Medidas ≈ em pixels lógicos.

### Raios
| Elemento | Raio |
|---|---|
| Cards (login, KPI, tabela, gráfico), modal | 12px (`rounded-xl`) |
| Inputs, selects, botões, item de menu, caixas de ícone, avatar de iniciais quadrado, área de upload | 6–8px (`rounded-md`/`rounded-lg`) |
| Container de conteúdo inset (app shell) | 12px (`rounded-xl`) |
| Badges de status da tabela ("Ativo", "Suspenso") | `rounded-full` |
| Badges de métrica ("Normal", "Anomalia") | 8px (`rounded-lg`) |
| Avatar do usuário (menu) | `rounded-full` |

### Alturas de controles
| Controle | Altura |
|---|---|
| Input / Select / Botão padrão | 40px (`h-10`) |
| Botão compacto ("Adicionar tenant", "Enviar imagem"), input de busca | 36px (`h-9`) |
| Abas (container) | 40px (`h-10`, `p-1`) |
| Item de menu lateral de configurações | 32px (`h-8`) |
| Linha de tabela | ≈60px |
| Cabeçalho de tabela | 48px (`h-12`) |
| Header da aplicação | 64px (`h-16`) |

### Espaçamentos recorrentes
| Contexto | Valor |
|---|---|
| Padding de página (conteúdo) | 32px (`p-8`) em telas de gestão; 16px (`p-4`) no dashboard |
| Padding interno de card | 16–24px (`p-4` a `p-6`); card de login `p-6` |
| Gap entre cards de KPI | 12–16px (`gap-3`/`gap-4`) |
| Label → input | 8px (`space-y-2`) |
| Entre campos de formulário | 16–24px (`space-y-4`/`space-y-6`) |
| Título → descrição (pares de título) | 4px (`space-y-1`) |
| Bloco de título → formulário | 24px |
| Menu lateral (configurações) → conteúdo | ≈48px (`gap-12`) |

### Bordas e sombras
- Bordas sempre 1px.
- Sombras: no máximo `shadow-sm` (cards de métrica e de KPI). Cards de conteúdo dependem de borda + contraste branco/`#F8FAFC`. Modal: `shadow-lg` padrão do Dialog.
- Divisores: `border-b border-input` sob o header da aplicação; `border-b border-border` entre linhas de tabela e seções de modal.

---

## 5. Iconografia

Biblioteca: **lucide-react**, traço 2px (padrão). Tamanhos: 16px em botões, badges e menus; 20px em caixas de ícone de KPI; 24px nos cards de métrica.

| Contexto | Ícone (lucide) | Cor |
|---|---|---|
| Toggle da sidebar (header) | `PanelLeft` | foreground |
| KPI total de atuadores / Corrente | `Zap` | primary |
| KPI ativos / badge Normal / Ativo / sucesso | `CircleCheck` | success |
| KPI em alerta / Suspenso | `CircleAlert` | warning |
| Badge Anomalia | `CircleAlert` | anomaly |
| KPI com problema | `CircleX` | danger |
| Tenants (KPI, modal) | `Building2` | primary |
| Usuários | `Users` | muted-foreground |
| Atuadores (coluna da tabela) | `Activity` | muted-foreground |
| Torque | `LoaderCircle` / `CircleDashed` | primary |
| Posição | `Gauge` | primary |
| Temperatura | `Thermometer` | primary |
| Vibração | `Waves` | primary |
| Tensão | `Signal` | primary |
| Busca | `Search` | muted-foreground/foreground |
| Ordenação de coluna | `ChevronsUpDown` | muted-foreground |
| Ações de linha | `Ellipsis` | muted-foreground |
| Select | `ChevronDown` | muted-foreground |
| Adicionar | `Plus` | primary-foreground |
| Mostrar senha | `Eye` / `EyeOff` | foreground |
| Upload | `Upload` (botão), `ImagePlus` (área) | primary-foreground / primary |
| Configuração (menu usuário) | `SlidersHorizontal` | foreground |
| Sair | `LogOut` | foreground |
| Fechar modal | `X` | muted-foreground |
| Avançar (CTA) | `ArrowRight` | primary-foreground |

Use o ícone mais próximo do lucide quando a correspondência acima não for exata, mas mantenha o mesmo ícone para o mesmo conceito em todo o produto.

---

## 6. Layout e estrutura de páginas (app shell)

Referência: `screens/02`, `03`, `05`, `06`.

```
┌──────────────────────────────────────────────────────────┐  ← fundo/sidebar em --primary (#1A3C85)
│ ┌──────────────────────────────────────────────────────┐ │
│ │ [PanelLeft]  Título da página               (h-16)   │ │  ← container inset: bg #F8FAFC, rounded-xl, m-2
│ ├──────────────────────────────────────────────────────┤ │  ← border-b #CBD5E1
│ │                                                      │ │
│ │   conteúdo da página                                 │ │
│ │                                                      │ │
│ └──────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

- Implementar com o `Sidebar` do shadcn em **`variant="inset"`** e `collapsible="offcanvas"`: a sidebar e a moldura usam `--primary`; o conteúdo (`SidebarInset`) fica num container com margem ≈8px, `rounded-xl`, fundo `--background`.
- Header (`h-16`, `px-4` ou alinhado ao padding da página): `SidebarTrigger` (`PanelLeft`) + título da página em 16px regular. Divisor inferior `border-input`.
- No dashboard o header pode não ter título (só o trigger).
- Nas telas mostradas a sidebar está recolhida; o conteúdo expandido da sidebar **não** está no design (ver seção 14).

### Grid de conteúdo
- Telas de gestão (Tenants): largura total com `p-8`; cabeçalho de página com título à esquerda e CTA primário à direita (`flex items-start justify-between`).
- KPIs: `grid grid-cols-1 sm:grid-cols-2 xl:grid-cols-4 gap-3`.
- Formulários de configurações: coluna de conteúdo com largura máx. ≈360–384px (`max-w-sm`), alinhada à esquerda.

---

## 7. Logo e marca

- **Logo oficial: Coester** (símbolo de onda com o nome atravessado). Referência visual em `screens/01-login.png`.
- Versão **branca** sobre fundo `--primary`. Em fundos claros, usar a versão azul (`#1A3C85`) do brand kit, se existir — nunca o branco sobre claro.
- Arquivo: usar o SVG oficial do repositório (ex.: `public/brand/coester-logo-white.svg` e `public/brand/coester-logo.svg`). **Não** reconstruir o logo em código nem extraí-lo do screenshot. Se o arquivo não existir, criar um placeholder com o mesmo tamanho e deixar `TODO: inserir logo oficial`.
- Login: logo centralizado, largura ≈200px (altura proporcional ≈150px), 24px acima do card.
- Sidebar expandida (quando existir): logo branco no topo, altura ≈32px.
- Componente único: `<BrandLogo variant="white" | "primary" className=... />` — todas as telas usam este componente.

---

## 8. Componentes

Cada componente abaixo indica a referência visual, a anatomia e as classes recomendadas.

### 8.1 Button
| Variante | Visual | Uso |
|---|---|---|
| `default` (primário) | bg `primary`, texto `primary-foreground`, 14px medium, `rounded-md`, `h-10 px-4` (compacto `h-9`) | Ação principal: "Entrar", "Salvar alterações", "Adicionar tenant", "Visualizar tenant" |
| `outline` | bg `background` (#F8FAFC), borda `border`, texto foreground semibold | Ação secundária: "Fechar" |
| `ghost` | sem fundo; hover `primary-soft` | Itens de menu, ações de linha (`Ellipsis`) |
| `link` | texto `primary`, sem sublinhado | "Previous" na paginação |

- Ícone à esquerda para ações de criar/enviar (`Plus`, `Upload`), à direita para navegar (`ArrowRight`). `gap-2`, ícone 16px.
- Botão de submit de formulário em largura total apenas no login; nas configurações tem largura do conteúdo, alinhado à esquerda.
- Hover: `bg-primary/90`. Disabled: `opacity-50 pointer-events-none`. Loading: `LoaderCircle` girando + texto mantido.

### 8.2 Input
- `h-10`, `rounded-md`, borda `border-input` (#CBD5E1), fundo `card` (#FFF), texto 14px foreground, placeholder muted-foreground, `px-3`.
- Focus: `ring-2 ring-ring/30 border-primary` (anel no primário).
- **Senha:** ícone `Eye` à direita dentro do campo (`pr-10`, botão ghost absoluto), alterna para `EyeOff`. Valor mascarado.
- **Busca:** variante com fundo `background` (#F8FAFC), borda `border` (#E2E8F0), `h-9`, ícone `Search` à esquerda (`pl-9`), placeholder "Buscar tenant…".
- Label sempre acima (14px medium), `space-y-2`.

### 8.3 Select
- Mesmo visual do Input (borda `input`, fundo branco, `h-10`), chevron `ChevronDown` muted à direita, placeholder em muted ("Cliente", "Filial", "Atuador", "Defina a sua permissão").
- Filtros do dashboard usam o próprio nome do filtro como placeholder (sem label externo).

### 8.4 Card
- **Card de conteúdo** (tabela, gráfico, login): bg branco, `rounded-xl`, borda `border` (ou sem borda no card de gráfico/login), padding `p-6` (login) / `p-4`–`p-6`.
- Cabeçalho do card: título (16–18px semibold) + descrição (14px muted) em `space-y-1`; ação opcional à direita (ex.: busca).

### 8.5 KPI Card (indicador agregado)
Referência: topo de `screens/05` e `screens/06`.
```
┌───────────────────────────────┐
│ Rótulo (14px muted)     [■ic] │  ← caixa de ícone 36px rounded-lg com bg soft do status
│ 4   (30px bold)               │
│ Texto de apoio (14px muted)   │  ← opcional (presente em Tenants)
└───────────────────────────────┘
```
- bg branco, borda `border` (Tenants) ou `input` (Dashboard), `rounded-xl`, `p-4`/`p-5`, `shadow-sm` opcional.
- Mapeamento de caixa de ícone: neutro → `bg-primary-soft text-primary`; sucesso → `bg-success-soft text-success`; alerta → `bg-warning-soft text-warning`; problema → `bg-danger-soft text-danger`; contagem neutra secundária (usuários) → `bg-background text-muted-foreground`.

### 8.6 Metric Card (leitura de sensor)
Referência: `screens/05`, grade de 6 cards.
```
┌──────────────────────────────────────────────┐
│ [ícone 24 primary]  68.7 Nm   (16px medium)   [✓ Normal] │
│                     Torque    (14px primary-muted-fg)    │
└──────────────────────────────────────────────┘
```
- bg branco, borda `input`, `rounded-xl`, `px-4 py-3`, `shadow-sm`, layout `flex items-center gap-4`, badge alinhado à direita (`ml-auto`).
- Grade: `grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 gap-x-16 gap-y-3` (no design há um espaço horizontal grande entre colunas e pequeno entre linhas).
- Formato de valor: número + unidade separados por espaço ("4.4 A", "245.5 V", "4.8 mm/s", "32.5%"). Use `°C` (o design mostra "C°" — corrigir para `°C`).

### 8.7 Badge
| Tipo | Visual |
|---|---|
| Status de entidade (tabela) | `rounded-full`, `h-6 px-2.5`, `gap-1.5`, ícone 16px; **Ativo:** `bg-success-soft border-success-border text-success` + `CircleCheck`; **Suspenso:** `bg-warning-soft border-warning-border text-warning` + `CircleAlert` |
| Status de leitura (métrica) | `rounded-lg`, bg branco, borda `border`, texto `muted-foreground` 14px, ícone colorido 16px; **Normal:** `CircleCheck` success; **Anomalia:** `CircleAlert` anomaly |

### 8.8 Tabs
Referência: `screens/05` ("Indicadores e Gráficos" / "Localização dos Atuadores").
- Container `h-10 p-1 rounded-lg bg-primary-soft`; triggers 14px medium `text-primary-strong`; trigger ativo `bg-card rounded-md shadow-sm`.
- Usar para alternar visões da mesma entidade; não usar como navegação principal.

### 8.9 Table
Referência: `screens/06`.
- Dentro de um Card (cabeçalho do card com título, descrição e busca à direita).
- Header: `h-12 bg-muted`, texto 13–14px semibold uppercase foreground, `border-b border-border`. Coluna ordenável com `ChevronsUpDown` à esquerda do rótulo.
- Linhas: ≈60px, `border-b border-border`, fundo branco, hover `bg-background`.
- Célula de entidade: avatar quadrado 36px `rounded-md bg-primary-soft text-primary` com iniciais (14px semibold) + nome (14–16px medium foreground) e slug abaixo (14px muted).
- Células numéricas com ícone muted à esquerda (`Users 1`, `Activity 0`).
- Datas em `dd/MM/yyyy` muted; valores especiais por extenso: "Sem limite", "Sem acesso".
- Última coluna: menu de ações (`Ellipsis` → DropdownMenu).
- Rodapé: "1-4 de 4" (muted, à esquerda) + paginação à direita.

### 8.10 Pagination
- "‹ Anterior" em `text-primary` (link) quando habilitado; muted quando desabilitado; página atual em caixa `size-9 rounded-md border-input bg-background`. (O design usa "Previous/Next"; usar **"Anterior/Próxima"** para manter pt-BR.)

### 8.11 Dialog (modal)
Referência: `screens/07`.
```
┌───────────────────────────────────────────┐
│ [■ ícone]  Título (18px semibold)     [X] │  ← header bg-muted (#F1F5F9), p-6
│ Descrição (14px muted)                    │
├───────────────────────────────────────────┤  ← border-b border
│              ( ✓ )  círculo 56px          │  ← corpo bg-background (#F8FAFC), centralizado
│      Título do resultado (18–20px semib.) │
│      Mensagem (14–15px foreground)        │
├───────────────────────────────────────────┤  ← border-t border
│                      [Fechar] [Ação  →]   │  ← rodapé bg-background, botões à direita, gap-4
└───────────────────────────────────────────┘
```
- Largura ≈540px (`sm:max-w-[540px]`), `rounded-xl`, borda `border`, sem padding no container (seções com padding próprio).
- Caixa de ícone do header: 32–36px `rounded-md bg-primary-soft text-primary`.
- Modal de sucesso: círculo `size-14 rounded-full bg-success-soft` com `CircleCheck` 28px `text-success`.
- Botões: secundário outline à esquerda, primário à direita com `ArrowRight`.

### 8.12 Dropdown do usuário
Referência: `screens/04`.
- Cabeçalho: avatar circular 40px `bg-input` (#CBD5E1) com iniciais foreground + nome (14px semibold) + e-mail (12–13px muted).
- Separadores finos (`bg-border`, pode estar a ~50% de opacidade).
- Itens 14px com ícone 16px à esquerda: "Configuração" (`SlidersHorizontal`), "Sair" (`LogOut`).

### 8.13 Upload de imagem (avatar)
Referência: `screens/02`.
- Área 80×80px `rounded-lg border border-dashed border-primary bg-primary-soft` com `ImagePlus` 24px primary centralizado.
- À direita: label "Foto de perfil" (14px medium), botão primário compacto "Enviar imagem" com `Upload`, hint "PNG ou JPG - max. 2MB" (14px muted).
- Após upload, a área mostra a imagem recortada (`object-cover`), mantendo o raio.

### 8.14 Navegação secundária (configurações)
Referência: `screens/02`, `03`.
- Coluna à esquerda ≈224px (`w-56`), lista vertical: "Perfil", "Segurança", "Aparência".
- Item: `h-8 px-3 rounded-md`, 14–16px regular foreground; **ativo** `bg-primary-soft`; hover `bg-primary-soft/60`.

---

## 9. Padrões de tela (templates)

### 9.1 Login (`screens/01`)
- Fundo de tela cheia `bg-primary`; tudo centralizado vertical e horizontalmente.
- Logo Coester branco (≈200px) → 24px → Card branco `w-full max-w-sm` (384px), `rounded-xl`, `p-6`, sem borda.
- Card: título "Entre na sua conta" + descrição → 24px → campos Email e Senha (`space-y-6`) → botão "Entrar" `w-full h-10`.
- Erros de autenticação: mensagem acima do botão em `text-anomaly` 14px, sem apagar o e-mail digitado.

### 9.2 Configurações do usuário (`screens/02`, `03`)
- Header da aplicação com título "Configurações do Usuário".
- Layout duas colunas: navegação secundária (8.14) + conteúdo.
- Conteúdo: título de seção (20–24px) + descrição muted → formulário em coluna única `max-w-sm` → botão "Salvar alterações" (primário, largura do conteúdo).
- **Perfil:** upload de foto, Nome completo, E-mail, Permissão (Select).
- **Segurança:** Senha atual, Nova senha (ambas com `Eye`), Confirmar nova senha.
- **Aparência:** não desenhada (ver seção 14).

### 9.3 Dashboard de monitoramento (`screens/05`)
Ordem vertical, `space-y-4`:
1. Linha superior: Tabs à esquerda; Select "Cliente" à direita (≈240px).
2. Grade de 4 KPI cards (total / ativos / alerta / problema).
3. Linha de filtros: Selects "Filial" e "Atuador" (≈260px cada) à esquerda; "Última atualização: 5 minutos atrás" à direita em `text-primary-muted-fg` 14px, alinhado à base.
4. Grade 3×2 de Metric Cards.
5. Card de gráfico (seção 10).
- Filtros são hierárquicos: Cliente → Filial → Atuador (ao trocar o pai, limpar os filhos).

### 9.4 Listagem administrativa (`screens/06`)
1. Cabeçalho de página: H1 + descrição muted à esquerda; CTA primário com `Plus` à direita.
2. Grade de 4 KPI cards com texto de apoio.
3. Card com tabela (cabeçalho do card + busca, tabela, rodapé com contagem e paginação).
- Criar um novo item abre Dialog de formulário; ao concluir, Dialog de sucesso (9.5).

### 9.5 Confirmação de sucesso (`screens/07`)
- Dialog padrão (8.11) com header contextual (ícone da entidade + "Tenant criado" + descrição), corpo com ícone de sucesso + título + mensagem que nomeia a entidade e o próximo passo, rodapé com "Fechar" e ação de navegação "Visualizar tenant →".

---

## 10. Gráficos

Referência: `screens/05` — "Torque em relação a posição".

- Container: Card branco `rounded-xl`, `p-6`; cabeçalho com título (18px semibold) + subtítulo muted ("Últimos 10 registros"); divisor `border-input` abaixo do cabeçalho.
- Legenda no **topo à esquerda**, marcadores quadrados 12px, texto 14px muted.
- Recharts `LineChart`, altura ≈400px, `ResponsiveContainer` width 100%.
  - Séries: `type="monotone"`, `strokeWidth={2}`, cores `--chart-1` (Subida) e `--chart-2` (Descida).
  - Pontos: `r={7}`, `fill` = cor da série, `fillOpacity={0.8}`, sem stroke.
  - `CartesianGrid vertical={false} stroke="var(--chart-grid)"`.
  - Eixos sem linha e sem ticks (`axisLine={false} tickLine={false}`), ticks 12px muted.
  - Rótulos de eixo: Y vertical "Torque (Nm)", X "Posição (%)" centralizado, 14px muted.
  - Tooltip no padrão `ChartTooltip` do shadcn (card branco, borda `border`, `rounded-lg`, valores com unidade).
- Novas séries: usar `--chart-1`, `--chart-2` e só então criar `--chart-3+` derivados — perguntar antes de introduzir cor nova.

---

## 11. Estados e semântica de status

| Conceito | Rótulo | Cores | Ícone |
|---|---|---|---|
| Entidade ativa / leitura normal / operação concluída | "Ativo", "Normal" | success | `CircleCheck` |
| Requer atenção / suspenso / alerta | "Suspenso", "Em alerta" | warning | `CircleAlert` |
| Leitura fora da faixa | "Anomalia" | anomaly | `CircleAlert` |
| Falha / problema | "Com problema" | danger | `CircleX` |
| Neutro / contagem | — | primary-soft / primary | ícone da entidade |

Estados de interação (aplicar a todos os componentes interativos):
- **Hover:** primário `/90`; ghost `bg-primary-soft`; linhas de tabela `bg-background`.
- **Focus visível:** `ring-2 ring-ring/30` + `border-primary` em campos; nunca remover outline sem substituto.
- **Disabled:** `opacity-50`, cursor `not-allowed`.
- **Erro de campo:** borda `anomaly`, mensagem 12–14px `text-anomaly` abaixo do campo, `aria-invalid`.
- **Carregando:** Skeleton (`bg-muted animate-pulse rounded-md`) com as mesmas dimensões dos cards/linhas.
- **Vazio:** dentro do card, ícone da entidade em caixa `primary-soft`, frase do que falta e CTA primário (ex.: "Nenhum tenant cadastrado" + "Adicionar tenant").
- **Atualização de dados ao vivo:** manter o texto "Última atualização: X minutos atrás" (tempo relativo, pt-BR).

---

## 12. Conteúdo e microcopy (pt-BR)

- Idioma: português do Brasil em toda a interface. Não deixar termos em inglês na UI (corrigir "Previous/Next" do design para "Anterior/Próxima"; "Tenants" é termo de domínio aceito).
- Sentence case. Verbos no imperativo nos CTAs, dizendo exatamente o que acontece: "Salvar alterações", "Enviar imagem", "Adicionar tenant", "Visualizar tenant", "Entrar".
- A ação mantém o nome no fluxo: botão "Adicionar tenant" → modal "Tenant criado".
- Descrições curtas sob títulos explicam o propósito da tela ("Atualize as informações da sua conta").
- Placeholders orientam o preenchimento ("Digite o seu nome completo", "Confirme a senha").
- Datas `dd/MM/yyyy`; decimais no padrão do design (ponto) para leituras técnicas — se o produto adotar locale pt-BR completo, padronizar vírgula em todo lugar, nunca misturar.
- Mensagens de erro dizem o que houve e como resolver, sem pedir desculpas.

---

## 13. Acessibilidade e responsividade

- Contraste: texto `#64748B` sobre branco/`#F8FAFC` passa AA para 14px+; não usar muted em textos menores que 12px.
- Todos os botões só-ícone (`PanelLeft`, `Ellipsis`, `Eye`, `X`) com `aria-label` em pt-BR.
- Inputs com `<Label htmlFor>`; badges de status com texto (nunca só cor).
- Navegação por teclado completa em Tabs, Select, Dialog (foco preso e retorno ao gatilho) e DropdownMenu.
- Respeitar `prefers-reduced-motion`.
- Breakpoints: KPIs 1 → 2 → 4 colunas; Metric cards 1 → 2 → 3; configurações viram uma coluna com a navegação secundária em abas horizontais abaixo de `md`; tabela com `overflow-x-auto` dentro do card; filtros do dashboard empilham em telas estreitas; login mantém `max-w-sm` com `px-4`.

---

## 14. Lacunas do design (o que não está especificado)

Estas partes **não aparecem** nas telas de referência. Ao implementá-las, siga os tokens deste documento, mantenha o estilo e sinalize no PR (`[design-pendente]`) para validação:

- Sidebar expandida (itens de navegação, estado ativo, logo). Proposta: fundo `--primary`, itens 14px `text-primary-foreground/80`, ativo `bg-white/10 text-primary-foreground`, ícones lucide 16px.
- Aba "Localização dos Atuadores" (mapa).
- Seção "Aparência" e modo escuro — nenhum token dark foi definido; **não criar dark mode** sem design.
- Formulário de criação de tenant, menus de ação da linha, toasts, estados de erro/vazio/carregando.
- Tela de recuperação de senha.

---

## 15. Checklist de revisão para PRs de UI

- [ ] Nenhuma cor fora dos tokens da seção 2 (buscar por `#` e classes de paleta Tailwind cruas no diff).
- [ ] Borda correta: `input` em campos e cards de métrica; `border` em cards de conteúdo, tabela, modal.
- [ ] Alturas: controles `h-10` (compactos `h-9`); header `h-16`.
- [ ] Raios: cards `rounded-xl`; controles `rounded-md`; badges de status `rounded-full`.
- [ ] Ícones lucide, tamanhos 16/20/24, mesmo ícone para o mesmo conceito.
- [ ] Logo via `<BrandLogo />` com o arquivo oficial Coester.
- [ ] Textos em pt-BR, sentence case, CTAs descritivos.
- [ ] Status com fundo + borda + ícone + texto.
- [ ] Estados hover/focus/disabled/loading/vazio implementados.
- [ ] Responsivo conforme seção 13 e comparado visualmente com a imagem em `./screens/`.
