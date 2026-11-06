# Diário — 2026-11-06 — Compilador para bytecode e máquina virtual

| Campo | Valor |
|-------|-------|
| Data | 2026-11-06 |
| Responsável | BeathAzreal |
| Ferramentas | Claude Code (assistente de IA), GCC 13.3, Clang 18.1, CMake 3.28, AddressSanitizer/UBSan, Python 3 (`fractions`, `math`) para testes diferenciais, `localedef` (locale pt_BR de teste) |
| Etapa do roadmap | v0.1 — núcleo, item 4 (compilador e VM) |
| Branch | `feat/vm` → PR para `dev/mfl-core-v0.1` |
| Antes desta etapa | PR #4 (verificação semântica) integrado (`8ff28d0`), CI aprovado nas 4 plataformas |

## 1. Objetivo da sessão

Fazer os programas MFL **executarem**: compilar a AST verificada para
bytecode, executá-lo em uma máquina virtual própria e fazer o teste de
conformidade nº 1 (energia de soldagem) imprimir `480 J/mm`.

## 2. Procedimento executado

1. **Números independentes de *locale*** (`src/runtime/number.c`),
   implementando o ADR-011:
   - leitura: caminho rápido exato (algoritmo de Clinger), com mantissa até
     2⁵³ e potência de 10 até 10²²;
   - escrita: a menor representação decimal que, lida de volta, dá o mesmo
     *double*. Os dígitos são extraídos sem depender do separador decimal.
2. **Bytecode** (`src/compiler/chunk.c`):
   - 28 instruções, com tabela de constantes, tabela de unidades e
     listagem legível (`mfl --bytecode`);
   - o compilador calcula a profundidade máxima exata da pilha.
3. **Compilador** (`src/compiler/compiler.c`):
   - variáveis viram posições numeradas;
   - literais com unidade viram constantes (magnitude como escrita + unidade);
   - `and`/`or` com avaliação em curto-circuito.
4. **Máquina virtual** (`src/vm/vm.c`):
   - pilha de valores; a saída vai por uma função de escrita fornecida
     pelo programa hospedeiro (pensando em jogos);
   - valida todo o bytecode que executa.
5. **Representação exata dos fatores de unidade** (ver §4): num/den × 10^k.
6. **CLI:** `mfl arquivo.mfl` executa; `mfl --bytecode` mostra o código.
7. **Testes:**
   - unitários: `test_number` e `test_vm`;
   - conformidade: `conformance_*.mfl` registrados automaticamente;
   - execução dos exemplos e listagem do bytecode.

## 3. Resultados verificados

```
$ mfl examples/welding/heat_input.mfl
480 J/mm
```

O arquivo esperado `tests/language/conformance_heat_input.expected` foi
escrito em 2026-11-05, antes de existir interpretador, e **não foi
alterado**; o programa produz exatamente esse conteúdo.

Exemplo térmico (`examples/thermal/steel_properties.mfl`):

```
12.461921905289394 mm^2/s      # difusividade do aço
0.49847687621157577            # número de Fourier (< 0,5: passo explícito estável)
0.00025 m^3/s 2.4 kW           # vazão de gás e potência do arco
```

Os três valores coincidem dígito a dígito com o mesmo cálculo feito
independentemente em Python, também em doubles IEEE 754.

| Verificação | Resultado |
|-------------|-----------|
| Build GCC (Debug e Release) e Clang (Release) com `-Werror` | Sem warnings |
| CTest: 22 testes (6 unitários, 3 de conformidade, 3 exemplos executados, 1 de bytecode, 9 das etapas anteriores) | 22/22 nos três builds |
| `test_number`: 200000 literais aleatórios comparados com `strtod`; 200000 doubles aleatórios: a saída volta ao mesmo valor e é mínima | 0 divergências |
| `test_vm`: saídas, conversões, lógica, todos os limites de memória do compilador e da VM, 20000 bytecodes danificados (19888 rejeitados, nenhum travamento) | 0 falhas |
| Teste sob *locale* pt_BR (vírgula decimal): leitura, escrita e pipeline completo | Saída idêntica (`0.8`, `480 J/mm`) |
| **Teste diferencial:** 50000 expressões aritméticas aleatórias, MFL × Python | 0 divergências (após corrigir o modelo, ver §4) |
| **Conversão de unidades contra aritmética racional exata:** 4000 somas de até 5 grandezas em unidades mistas | Pior erro relativo 5,0e-16 (2,3 ulp) |
| ASan + UBSan: suíte, testes diferenciais, 3000 programas da gramática (todos os 59 que passam na verificação executam com sucesso), 300 arquivos aleatórios | Nenhum erro de memória |
| CI no GitHub Actions | Ver PR |

## 4. Problemas encontrados

| Problema | Solução |
|----------|---------|
| **Precisão de conversão:** `(1 g/cm^3).to(kg/m^3)` imprimia `999.9999999999999`, porque os fatores eram multiplicados em ponto flutuante ((1e-2)³ ≠ 1e-6 exato) | Fatores de unidade guardados de forma exata (num/den × 10^k), com a potência de 10 aplicada em uma operação. Agora imprime `1000 kg/m^3`, e conversões só de prefixo têm arredondamento correto |
| Primeira versão: o valor era guardado em SI, e `print(0.1 mm)` poderia não imprimir exatamente `0.1 mm` | Valores guardam a magnitude **na unidade de exibição**; só operações entre unidades diferentes passam pelo SI |
| Duas falhas latentes de segurança de memória no meu código, corrigidas antes de compilar: capacidade do array de linhas copiada da do código; escrita na pilha antes da verificação de espaço em `print()` | Corrigidas; depois, os testes com bytecode danificado e todos os limites de memória passaram sob AddressSanitizer |
| 2 divergências no primeiro teste diferencial | Eram do **meu modelo em Python**: `math.pow(0, -2)` lança exceção, mas em C99 (que o MFL segue) o resultado é `inf`. Modelo corrigido; 0 divergências em 50000 expressões |
| Frases de status desatualizadas no README e em `architecture.md` desde as etapas anteriores ("CLI stub that only reports its version") | Corrigidas nesta etapa |

## 5. Decisões tomadas

Ver ADR-020 a ADR-023 em [`../decisoes.md`](../decisoes.md).

## 6. Uso de IA nesta sessão

Código, testes e registro gerados com o Claude Code a partir dos
requisitos do autor. Ver [`../uso-de-ia.md`](../uso-de-ia.md).

## 7. Próximos passos

1. API C de embutir (`mfl_state_new`, `mfl_eval_string`, `mfl_get_quantity`):
   o primeiro passo para integrar com o SoldamatGamification.
2. v0.2: funções, `if`/`while`/`for`, arrays.
3. Módulo numérico: solucionador 1D de condução de calor e verificação
   contra a solução analítica (plano de validação, etapas 2 a 4).
