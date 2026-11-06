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

## ADR-010 — Quebras de linha como separadores, decididas no lexer

- **Data:** 2026-11-06 · **Status:** Aceita
- **Contexto:** a especificação usa quebra de linha como separador de
  comandos, mas permite continuar uma expressão dentro de `()`/`[]` ou
  depois de um operador.
- **Alternativas:** exigir `;` em todo comando; deixar todas as quebras
  para o parser decidir.
- **Decisão:** o lexer emite um único token `NEWLINE` por sequência de
  quebras e o suprime dentro de `()`/`[]`, depois de operadores e no início
  do arquivo. Uma linha que **começa** com operador não continua a anterior.
- **Justificativa:** a sintaxe fica próxima da notação de engenharia, sem
  `;`, e o parser fica mais simples. A regra é a mesma de linguagens como Go
  e Python (dentro de parênteses).

## ADR-011 — O lexer não converte números nem strings

- **Data:** 2026-11-06 · **Status:** Aceita
- **Decisão:** o lexer valida apenas o formato dos literais e devolve o
  trecho exato do código. A conversão para `double` fica para uma etapa
  posterior.
- **Justificativa:** `strtod` depende do *locale* do programa hospedeiro
  (um jogo configurado para pt-BR usaria `,` como separador decimal), o
  que quebraria resultados científicos. A conversão será feita por uma
  rotina própria, independente de *locale* e com arredondamento correto.

## ADR-012 — Identificadores ASCII e posições em pontos de código

- **Data:** 2026-11-06 · **Status:** Aceita
- **Decisão:** na v0.1, identificadores são ASCII; caracteres Unicode são
  aceitos só em strings e comentários. As colunas contam pontos de código
  Unicode, não bytes.
- **Consequências:** unidades como `µm` e `Ω` precisam ser escritas como
  `um` e `ohm` por enquanto. As mensagens de erro apontam a coluna que o
  usuário vê no editor.

## ADR-013 — Em um literal com unidade, `*` e `/` seguidos de nome pertencem à unidade

- **Data:** 2026-11-06 · **Status:** Aceita
- **Contexto:** `5 mm/s` precisa ser uma quantidade, mas `5 mm / t` é
  ambíguo: pode ser milímetros por `t` ou `5 mm` dividido pela variável `t`.
- **Alternativas:** (a) espaços com significado (`mm/s` × `mm / s`);
  (b) unidade sempre entre colchetes (`5[mm/s]`); (c) a regra da
  especificação: depois de um número, `*` e `/` seguidos de nome ou `(`
  continuam a unidade.
- **Decisão:** (c), como já previa a especificação. `*` ou `/` seguidos de
  número encerram a unidade (`5 mm * 2` é `(5 mm)·2`). Para dividir por uma
  variável, escreve-se `(5 mm) / t`.
- **Justificativa:** mantém a notação de engenharia (`7850 kg/m^3`) sem
  sintaxe extra. A armadilha `5 mm / t` fica documentada, e a etapa
  semântica vai acusar "unidade desconhecida `t`".

## ADR-014 — AST em arena e recuperação de erros em "modo pânico"

- **Data:** 2026-11-06 · **Status:** Aceita
- **Decisão:** todos os nós são alocados em uma região liberada de uma só
  vez. Após um erro, o parser ignora o resto do comando e continua no
  próximo, até 50 erros.
- **Justificativa:** sem liberação nó a nó, não há vazamento nem ponteiro
  solto, o que é importante para embutir a linguagem em jogos. Vários erros
  por execução ajudam estudantes. O limite de memória configurável permitiu
  testar todos os pontos de falta de memória.

## ADR-015 — Palavra-chave de comando no início da linha sempre encerra o comando anterior

- **Data:** 2026-11-06 · **Status:** Aceita
- **Contexto:** com a regra de continuação dentro de `()`, um parêntese
  esquecido engolia o resto do arquivo (ver diário de 2026-11-06, parser).
- **Decisão:** linhas que começam com `let var fn if while for return
  import` encerram o comando anterior, mesmo dentro de um grupo aberto ou
  depois de um operador.
- **Consequências:** programas válidos não mudam, porque essas palavras não
  podem aparecer dentro de expressões. Se no futuro alguma delas puder
  aparecer em uma expressão (por exemplo, `fn` anônima), esta regra precisa
  ser revista.

## ADR-016 — Análise dimensional estática (antes da execução)

- **Data:** 2026-11-06 · **Status:** Aceita
- **Contexto:** o ADR-003 previa verificar dimensões em tempo de execução.
  Na v0.1 não há funções nem desvios, então o tipo de cada expressão é
  conhecido antes de rodar.
- **Decisão:** um verificador semântico infere tipo e dimensão de todas as
  expressões e recusa o programa inconsistente antes da execução. A
  verificação em tempo de execução continua prevista para quando houver
  tipagem gradual (funções sem anotação, v0.2+).
- **Justificativa:** o erro aparece antes de qualquer simulação longa e
  aponta a linha. Isso é importante para estudantes e para resultados
  científicos.

## ADR-017 — Uma `var` mantém o tipo e a dimensão do valor inicial

- **Data:** 2026-11-06 · **Status:** Aceita
- **Alternativas:** permitir que a variável mude de tipo (dinâmico).
- **Decisão:** `var T = 300 K` seguido de `T = 5 s` é erro.
- **Justificativa:** uma grandeza física não muda de natureza ao longo de
  um modelo. Isso também mantém a análise da ADR-016 simples e exata.

## ADR-018 — Regras estritas: sem "verdadeiro" numérico, sem comparação entre tipos

- **Data:** 2026-11-06 · **Status:** Aceita
- **Decisão:** `and`, `or` e `not` exigem `Bool`. `==` exige o mesmo tipo
  (e a mesma dimensão), exceto comparação com `nil`. Expoente de grandeza
  com dimensão deve ser um inteiro literal.
- **Justificativa:** comparações como `1 == "a"` são sempre falsas e quase
  sempre um engano. O expoente literal garante que a dimensão do resultado
  seja conhecida.
- **Consequência:** `(2 m)^n`, com `n` variável, não é aceito na v0.1.

## ADR-019 — Dimensões exibidas em unidades de base SI; `degC` adiado

- **Data:** 2026-11-06 · **Status:** Aceita
- **Decisão:** mensagens e `mfl --types` mostram dimensões em unidades de
  base (`kg*m/s^2`), nunca um nome derivado. `degC` gera "ainda não
  suportado; use K". Prefixos valem também para `L` (`mL`).
- **Justificativa:** uma mesma dimensão tem vários nomes (J/m e N), e
  mostrar "N" para uma energia por comprimento confundiria. A temperatura
  com deslocamento (°C) exige regras próprias (`docs/units.md` §5), ainda
  em aberto.

## ADR-020 — Valores guardam a magnitude na unidade de exibição

- **Data:** 2026-11-06 · **Status:** Aceita (substitui a representação
  descrita no rascunho de `docs/architecture.md`)
- **Alternativas:** guardar tudo em SI e converter só na impressão.
- **Decisão:** cada número guarda a magnitude na sua unidade de exibição.
  Operações entre unidades diferentes convertem para SI; `+ - %` entre
  operandos da mesma unidade mantêm a unidade (`1 mm + 2 mm` → `3 mm`).
- **Justificativa:** `print(0.1 mm)` imprime exatamente `0.1 mm`, sem
  passar por conversões com arredondamento. A regra de manter a unidade
  comum é uma extensão de `docs/units.md` §4, documentada lá.

## ADR-021 — Fatores de unidade exatos (num/den × 10^k)

- **Data:** 2026-11-06 · **Status:** Aceita
- **Contexto:** multiplicar fatores em ponto flutuante fazia
  `(1 g/cm^3).to(kg/m^3)` resultar em `999.9999999999999`.
- **Decisão:** cada unidade guarda inteiros `num`, `den` e um expoente
  decimal `k`. A conversão aplica a parte inteira e depois 10^k em uma única
  operação exata.
- **Consequência:** conversões entre prefixos decimais têm arredondamento
  correto. Erro máximo medido em somas com unidades mistas: 2,3 ulp.

## ADR-022 — Semântica IEEE 754 e ausência de erros em tempo de execução na v0.1

- **Data:** 2026-11-06 · **Status:** Aceita
- **Alternativas:** erro em tempo de execução para divisão por zero.
- **Decisão:** divisão por zero dá `inf`, `-inf` ou `nan`, como em C,
  Fortran e NumPy. `%` segue o `fmod` do C (sinal do dividendo). Como nomes,
  tipos e dimensões são verificados antes, um programa v0.1 aceito não tem
  erros em tempo de execução.
- **Justificativa:** compatível com o software científico que os usuários
  conhecem, e a VM fica simples e rápida. Pode ser revisto quando houver
  funções e laços (v0.2).

## ADR-023 — Leitura e escrita de números próprias, independentes de *locale*

- **Data:** 2026-11-06 · **Status:** Aceita (implementa o ADR-011)
- **Decisão:** a leitura usa o caminho exato de Clinger e, nos casos raros,
  o `strtod` com o separador adaptado ao *locale*. A escrita usa a menor
  representação que volta ao mesmo valor, com notação exponencial fora de
  1e-4 ≤ |x| < 1e16.
- **Verificação:** 200000 literais comparados com `strtod`; 200000 doubles
  com ida e volta; execução sob pt_BR com vírgula decimal; 50000
  expressões comparadas com o `repr` de Python (algoritmo de Gay).

## ADR-024 — API de embutir: entradas com unidade, scripts compilados uma vez

- **Data:** 2026-11-06 · **Status:** Aceita
- **Contexto:** jogos precisam executar o mesmo modelo muitas vezes, com
  valores escolhidos pelo jogador, e mostrar resultados na sua própria
  interface.
- **Decisão:**
  - entradas com unidade (`mfl_set_number`) entram no script como
    variáveis imutáveis com dimensão conhecida, e por isso a análise
    dimensional vale também para elas;
  - um `MflScript` é compilado uma vez e executado muitas vezes;
  - resultados são lidos em qualquer unidade compatível (`mfl_get_number`);
  - não há estado global, nem `exit()`;
  - a saída de `print` vai para uma função do hospedeiro.
- **Diferenças do rascunho:** `mfl_eval_file` foi retirado e o alocador do
  hospedeiro ficou para depois (por ora há só um limite de memória).
- **Consequência:** se uma entrada muda de dimensão, o script precisa ser
  recompilado (`MFL_ERR_INPUTS_CHANGED`).

## ADR-025 — Multiplicação e divisão continuam convertendo para SI primeiro

- **Data:** 2026-11-06 · **Status:** Aceita (experimento alternativo
  rejeitado)
- **Alternativa testada:** multiplicar magnitudes e combinar fatores de
  unidade exatos, convertendo uma vez no final.
- **Resultado:** com 4500 casos aleatórios contra aritmética exata, o
  arredondamento correto caiu de 64,5% para 59,1%, com valores típicos.
- **Decisão:** manter a conversão para SI primeiro. Ver o diário de
  2026-11-06 (API).

