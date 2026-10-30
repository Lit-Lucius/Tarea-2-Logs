# Tarea 2 — Árboles B y Bε (CC4102)

Implementación en C++17 de un **árbol B** y de dos variantes de **árbol Bε**, con
inserción de pares `(llave int32, valor float)`, búsqueda por rango, simulación de
I/Os y serialización a disco en bloques de exactamente **4096 bytes**.

| Estructura | Parámetros | Tamaño de nodo |
|---|---|---|
| Árbol B | `b = 340` | 4096 B |
| Bε (ε ≈ 0.25) | `c = 4`, `lmax = 506` | 4096 B |
| Bε (ε ≈ 0.75) | `c = 126`, `lmax = 384` | 4096 B |

El código no usa librerías externas (solo la biblioteca estándar de C++17).

---

## 1. Requisitos

- **Linux** (probado también en Windows con MinGW-w64).
- `g++ >= 9` o `clang++ >= 10` con soporte de C++17 (`std::filesystem`).
- `make` (opcional).
- El archivo `datos.bin` (512 MB, 67 108 864 pares de 8 bytes) en la raíz del
  proyecto o en la ruta que se indique con `--data`.
- Para los gráficos: Python 3 con `matplotlib`.

En Ubuntu/Debian:

```bash
sudo apt update && sudo apt install g++ make python3-matplotlib
```

---

## 2. Compilación

Con `make`:

```bash
make
```

o directamente:

```bash
g++ -O2 -std=c++17 -Wall -Wextra -o tarea2 src/btree.cpp src/main.cpp
```

> Si usas un GCC antiguo (< 9) agrega `-lstdc++fs`.

---

## 3. Uso

### 3.1 Autotest (recomendado antes de experimentar)

```bash
./tarea2 selftest
```

Inserta 120 000 pares aleatorios en los tres árboles, verifica todas las
invariantes (capacidades, orden, rangos de los hijos, hojas a igual profundidad,
cantidad de elementos) y compara 200 búsquedas por rango contra fuerza bruta.
Debe terminar con `Todas las pruebas pasaron.`

### 3.2 Experimento principal

```bash
./tarea2 experiment --data datos.bin --out resultados
```

Para cada `N ∈ {2^15, …, 2^26}`:

1. Lee los primeros `N` pares de `datos.bin`.
2. Construye los tres árboles, midiendo tiempo e I/Os simuladas.
3. Serializa cada árbol a un archivo binario.
4. Ejecuta 50 búsquedas por rango `[l, l + 604800]` con `l` uniforme en
   `[1546300800, 1754006400]`, leyendo desde disco.
5. Escribe una fila por estructura en `resultados/resultados.csv`.

Opciones:

| Opción | Descripción | Default |
|---|---|---|
| `--data RUTA` | Archivo binario de entrada | `datos.bin` |
| `--out DIR` | Directorio de salida | `resultados` |
| `--alpha X` | Valor de α para los Bε | `0.5` |
| `--queries Q` | Búsquedas aleatorias por árbol | `50` |
| `--emin E` / `--emax E` | Rango de exponentes de `N` | `15` / `26` |
| `--keep` | Conserva el `.bin` temporal de cada árbol | borrar |

Ejemplo rápido (solo N pequeños):

```bash
./tarea2 experiment --data datos.bin --out resultados --emin 15 --emax 20 --queries 10
```

> El experimento completo tarda del orden de **3–5 minutos** en un equipo moderno
> y usa como máximo ~1,5 GB de RAM y ~1,2 GB de disco para el árbol temporal.

### 3.3 Barrido de α (bonus)

```bash
./tarea2 alpha --data datos.bin --out resultados --n 24 --queries 20
```

Construye un Bε(0.25) con `N = 2^24` para
`α ∈ {0.1, 0.2, …, 0.8}` y escribe `resultados/alpha.csv`.

---

## 4. Salidas

```
resultados/
├── resultados.csv          # métricas para cada estructura y N
├── alpha.csv               # barrido de alpha (modo alpha)
├── rango_B_N2e20.csv       # búsqueda fija [FIXED_LO, FIXED_LO+604800] para N=2^20
├── rango_B_N2e26.csv       # la misma búsqueda para N=2^26
├── rango_Be025_N2e20.csv
├── ...
└── tree_tmp.bin            # solo con --keep
```

`resultados.csv` tiene las columnas:

```
estructura,N,alpha,tiempo_creacion_ms,ios_insercion,lecturas_insercion,
escrituras_insercion,nodos,tamano_bytes,tiempo_busqueda_ms,ios_busqueda,
promedio_resultados
```

`ios_insercion` es la suma de `lecturas_insercion` y `escrituras_insercion`.

Las búsquedas fijas se guardan para dos `N` distintos y sirven para el gráfico de
"misma búsqueda, distinto `N`" (los resultados deben coincidir en la ventana de
tiempo cubierta por ambos).

---

## 5. Gráficos

```bash
python3 scripts/plots.py resultados
```

Genera en `resultados/`:

- `fig_tiempo_creacion.png`
- `fig_ios_insercion.png`
- `fig_tamano.png`
- `fig_tiempo_busqueda.png`
- `fig_ios_busqueda.png`
- `fig_alpha.png` (si existe `alpha.csv`)
- `fig_rango_N20_vs_N26.png` (si existen los CSV de rango)

---

## 6. Estructura del código

```
src/
├── common.hpp   # KV, nodos de 4096 B, RamStore (disco simulado), FileStore
├── btree.hpp    # declaración del árbol B (con firmas documentadas)
├── btree.cpp    # inserción, split, serialización y búsqueda del árbol B
├── beps.hpp     # árbol Bε completo (template C, LMAX), header-only
└── main.cpp     # autotest, experimento principal, barrido de alpha
tests/
└── stress.cpp   # pruebas extra (órdenes aleatorio/ascendente/descendente, α)
scripts/
└── plots.py     # gráficos a partir de los CSV
```

Para compilar y correr las pruebas extra:

```bash
g++ -O2 -std=c++17 -o stress src/btree.cpp tests/stress.cpp
./stress
```

---

## 7. Decisiones de implementación

### 7.1 Layout de nodos

Los `static_assert` de `common.hpp` garantizan en tiempo de compilación que cada
nodo mide exactamente 4096 bytes, tanto para el árbol B como para las dos
configuraciones de Bε.

### 7.2 Conteo de I/Os

- **Construcción (RAM):** todo acceso al arreglo de nodos pasa por `RamStore`.
  Cada `read` cuenta una lectura y cada `write`/`append` una escritura. El total
  reportado es `lecturas + escrituras`.
- **Búsqueda (disco):** cada bloque leído con `FileStore::read` cuenta una
  lectura. No hay caché: cada visita a un nodo es una I/O.

### 7.3 Inserción en el árbol B

Se usa el esquema *top-down* del enunciado: si la raíz está llena se divide; al
descender, todo hijo lleno se divide **antes** de bajar. El split deja
`b/2 - 1 = 169` pares a la izquierda y `b/2 = 170` a la derecha, con el par
mediano subiendo al padre.

### 7.4 Inserción en el árbol Bε

- Toda inserción entra por la raíz y se agrega ordenadamente a su buffer.
- Cuando el buffer de la raíz se llena se ejecuta `flush`.
- `flush(v)`:
  1. Agrupa los pares del buffer por hijo (rangos `(llaves[i-1], llaves[i]]`).
  2. Calcula `f = ceil(k_buffer / (k_llaves + 1))` y
     `umbral = ceil(f · α)`.
  3. Procesa los hijos **de derecha a izquierda** cuyo grupo tenga al menos
     `umbral` pares.
  4. Extrae el grupo del buffer y lo transfiere al hijo.
- Si el hijo es una **hoja** y no cabe, se divide incorporando el lote completo:
  como `k_buffer + |lote| ≤ 2·lmax`, basta partir el arreglo mezclado en dos
  hojas de tamaño `≤ lmax`. La llave divisoria es la última llave de la hoja
  izquierda y se conserva allí (estilo B+).
- Si el hijo es **interno** y no cabe, se vacía su buffer recursivamente hasta
  que haya espacio.
- Si al absorber una división un nodo llega a `c` llaves, se divide y la división
  sube al padre; si es la raíz, se crea una nueva raíz (posición 0 del arreglo).
  Los pares pendientes se conservan siempre en los buffers de las dos mitades,
  por lo que no se pierden.

> El valor de `α` usado por defecto es **0.5**; se puede cambiar con `--alpha` y
> estudiarlo con el modo `alpha`.

### 7.5 Búsqueda por rango

- **Árbol B:** se revisan las llaves del nodo y se desciende por los hijos cuyos
  rangos intersectan `[l, u]`.
- **Bε:** además de descender por los hijos que intersectan el rango, se revisa
  el buffer del nodo actual (los pares pueden estar retenidos en cualquier nodo
  del camino raíz-hoja).

### 7.6 Memoria

Durante la construcción el árbol vive en un `std::vector` de nodos (el índice es
la posición en disco). Al terminar se escribe secuencialmente a un archivo y se
libera la RAM antes de buscar. Así, el peak de memoria es el de un solo árbol más
los `N` pares de entrada.

---

## 8. Notas y supuestos

- El dataset tiene **llaves únicas** (timestamps Unix), supuesto que simplifica
  el tratamiento de llaves repetidas en los separadores del Bε.
- Se asume una máquina **little-endian** (x86/ARM), consistente con el formato
  `int32 + float` de `datos.bin`.
- El árbol temporal se escribe en `--out/tree_tmp.bin` y se borra al terminar
  cada `N`, salvo que se use `--keep`.
- `selftest` y `tests/stress.cpp` validan invariantes y comparan las búsquedas
  contra fuerza bruta; se recomienda correrlos tras cualquier cambio.
