# Registro de Decisões de Projeto (ADR)

Cada decisão registra o contexto, as alternativas consideradas, a escolha e
as consequências. Formato inspirado em *Architecture Decision Records*
(Nygard, 2011).

Status possíveis: **Proposta**, **Aceita**, **Substituída por ADR-XXX**,
**Rejeitada**.

---

## ADR-001 — Implementação do núcleo em C11

- **Data:** 2026-11-05 · **Status:** Aceita
- **Contexto:** a linguagem precisa ser leve, multiplataforma, embutível em
  jogos e, no futuro, compilável para WebAssembly.
- **Alternativas:** C++17, Rust, Go, Python.
- **Decisão:** C11 usando apenas a biblioteca padrão e a `libm`.
- **Justificativa:** existe compilador para praticamente qualquer
  plataforma; a ABI estável facilita a API de embutir; a interoperabilidade
  com bibliotecas científicas em C e Fortran é direta; o executável é pequeno;
  há suporte maduro via Emscripten/WASI.
- **Consequências:** o gerenciamento de memória é manual, o que exige um
  coletor de lixo próprio e testes rigorosos (sanitizers no futuro).

## ADR-002 — Pipeline lexer → parser → AST → bytecode → VM

- **Data:** 2026-11-05 · **Status:** Aceita
- **Alternativas:** interpretador que percorre a árvore diretamente;
  compilador de passada única sem AST; compilação para C ou LLVM.
- **Decisão:** gerar uma AST completa e compilá-la para o bytecode de uma VM
  própria baseada em pilha.
- **Justificativa:** a AST permite verificar unidades e tipos antes da
  execução e dar mensagens de erro melhores; a VM é mais rápida que percorrer
  a árvore e é portátil (incluindo WebAssembly), sem depender de LLVM.

## ADR-003 — Unidades físicas como parte do valor

- **Data:** 2026-11-05 · **Status:** Aceita
- **Alternativas:** unidades como biblioteca externa; unidades só como
  anotação de tipo, sem efeito em tempo de execução.
- **Decisão:** o tipo `Quantity` guarda o valor em SI (`double`), um vetor de
  7 expoentes inteiros (as dimensões SI) e uma unidade de exibição.
- **Justificativa:** erros de unidade são frequentes em engenharia. A
  verificação é barata (soma de inteiros) e funciona mesmo sem anotações.
- **Consequências:** expoentes racionais (como `m^(1/2)`) ficam fora da v0.1;
  temperaturas com escala deslocada (`degC`) precisam de regra especial
  (questão em aberto).

## ADR-004 — `double` (IEEE-754 binary64) como tipo numérico padrão

- **Data:** 2026-11-05 · **Status:** Aceita
- **Justificativa:** é o padrão em computação científica e a precisão é
  suficiente para os modelos térmicos previstos.

## ADR-005 — Módulos científicos separados do núcleo

- **Data:** 2026-11-05 · **Status:** Aceita
- **Decisão:** `math`, `units`, `numerical` e `welding` ficam em `modules/` e
  se registram por uma API de módulos nativos.
- **Justificativa:** o núcleo fica pequeno e auditável; jogos podem embutir
  só o necessário; os modelos físicos evoluem sem mexer no interpretador.

## ADR-006 — Sem dependência de motores gráficos ou ecossistema JavaScript

- **Data:** 2026-11-05 · **Status:** Aceita
- **Decisão:** nenhuma dependência de React, JavaScript, Unity ou Unreal.
  A integração com jogos é feita exclusivamente pela API C.

## ADR-007 — CMake e CI com GitHub Actions desde o primeiro commit

- **Data:** 2026-11-05 · **Status:** Aceita
- **Decisão:** CMake ≥ 3.16; CI com matriz Linux (gcc e clang), macOS (clang)
  e Windows (MSVC), com warnings tratados como erro.
- **Justificativa:** garante desde o início que o código compila nas três
  plataformas-alvo.
- **Resultado verificado:** CI aprovado nos 4 jobs (run 42129409271).

## ADR-008 — Soldagem MIG/MAG como primeiro domínio de validação

- **Data:** 2026-11-05 · **Status:** Aceita
- **Justificativa:** existem soluções analíticas clássicas para verificação
  (Rosenthal), um modelo de fonte de calor consolidado (Goldak) e aplicação
  direta nos jogos educacionais (SoldamatGamification).
- **Detalhes:** [`docs/validation-plan.md`](../docs/validation-plan.md).

## ADR-009 — Teste de conformidade nº 1: energia de soldagem = 480 J/mm

- **Data:** 2026-11-05 · **Status:** Aceita
- **Decisão:** o programa de *heat input* com saída exata `480 J/mm` é o
  critério mínimo para lançar a v0.1.
