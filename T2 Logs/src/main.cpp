/**
 * @file main.cpp
 * @brief Programa principal: experimentos, barrido de alpha y autotest.
 *
 * Modos:
 *   ./tarea2 selftest
 *   ./tarea2 experiment --data datos.bin --out resultados [--alpha 0.5] [--queries 50] [--keep]
 *   ./tarea2 alpha      --data datos.bin --out resultados [--n 24] [--queries 20]
 */

#include "btree.hpp"
#include "beps.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Constantes del experimento
// ---------------------------------------------------------------------------

/** @brief Llave minima del dataset (2019-01-01 00:00:00 UTC). */
constexpr int32_t KEY_MIN = 1546300800;
/** @brief Llave maxima del dataset (2025-07-31 23:59:59 UTC aprox.). */
constexpr int32_t KEY_MAX = 1754006400;
/** @brief Tamano del rango de busqueda: una semana en segundos. */
constexpr int32_t RANGE_SPAN = 604800;
/** @brief Llave inicial usada para la busqueda fija que se grafica para dos N. */
constexpr int32_t FIXED_LO = 1637020800;

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

/**
 * @brief Lee los primeros n pares llave-valor de un archivo binario.
 * @param path Ruta del archivo datos.bin.
 * @param n    Cantidad de pares a leer.
 * @return Vector con los n pares leidos.
 */
std::vector<KV> readData(const std::string& path, size_t n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("No se pudo abrir " + path);

    in.seekg(0, std::ios::end);
    const std::streamoff bytes = in.tellg();
    in.seekg(0, std::ios::beg);
    if (bytes < 0 || (size_t)bytes / sizeof(KV) < n)
        throw std::runtime_error("El archivo " + path + " no contiene suficientes pares");

    std::vector<KV> data(n);
    in.read(reinterpret_cast<char*>(data.data()), (std::streamsize)(n * sizeof(KV)));
    if (!in) throw std::runtime_error("Error leyendo " + path);
    return data;
}

/** @brief Resultados de construir y consultar un arbol para un N dado. */
struct Metrics {
    std::string name;      ///< Nombre de la estructura.
    size_t n = 0;          ///< Cantidad de pares insertados.
    double buildMs = 0;    ///< Tiempo de construccion (ms).
    uint64_t insertReads = 0;  ///< Lecturas simuladas durante la insercion.
    uint64_t insertWrites = 0; ///< Escrituras simuladas durante la insercion.
    uint64_t insertIos = 0;///< I/Os simuladas durante la insercion (lecturas + escrituras).
    size_t nodes = 0;      ///< Cantidad de nodos usados.
    uint64_t fileBytes = 0;///< Tamano del arbol en disco (bytes).
    double searchMs = 0;   ///< Tiempo promedio de busqueda (ms).
    double searchIos = 0;  ///< I/Os promedio por busqueda.
    double results = 0;    ///< Promedio de pares retornados por busqueda.
};

/** @brief Escribe (o agrega) una fila de resultados en formato CSV. */
void appendCsv(const std::string& path, const Metrics& m, double alpha) {
    const bool exists = fs::exists(path) && fs::file_size(path) > 0;
    std::ofstream out(path, std::ios::app);
    if (!exists)
        out << "estructura,N,alpha,tiempo_creacion_ms,ios_insercion,lecturas_insercion,"
               "escrituras_insercion,nodos,tamano_bytes,tiempo_busqueda_ms,ios_busqueda,"
               "promedio_resultados\n";
    out << std::setprecision(10) << m.name << ',' << m.n << ',' << alpha << ',' << m.buildMs << ','
        << m.insertIos << ',' << m.insertReads << ',' << m.insertWrites << ',' << m.nodes << ','
        << m.fileBytes << ',' << m.searchMs << ',' << m.searchIos << ',' << m.results << '\n';
}

/**
 * @brief Construye un arbol, lo escribe a disco y ejecuta busquedas por rango.
 *
 * @tparam Tree Tipo del arbol (BTree o EpsTree<C,LMAX>).
 * @param name      Nombre de la estructura para los resultados.
 * @param tree      Arbol vacio a construir (se libera su RAM al escribir a disco).
 * @param data      Pares a insertar.
 * @param treePath  Archivo binario donde se serializa el arbol.
 * @param queries   Cantidad de busquedas aleatorias.
 * @param seed      Semilla del generador aleatorio.
 * @param dumpPath  Si no esta vacio, se guarda alli el resultado de la busqueda fija.
 * @return Metricas de construccion y busqueda.
 */
template <class Tree>
Metrics runTree(const std::string& name, Tree& tree, const std::vector<KV>& data,
                const std::string& treePath, int queries, uint64_t seed,
                const std::string& dumpPath) {
    Metrics m;
    m.name = name;
    m.n = data.size();

    // Construccion.
    const double t0 = nowMs();
    for (const KV& kv : data) tree.insert(kv);
    const double t1 = nowMs();

    m.buildMs = t1 - t0;
    m.insertReads = tree.insertReads();
    m.insertWrites = tree.insertWrites();
    m.insertIos = m.insertReads + m.insertWrites;
    m.nodes = tree.nodeCount();
    m.fileBytes = (uint64_t)m.nodes * BLOCK_BYTES;

    // Serializacion a disco y liberacion de la RAM.
    tree.saveTo(treePath);
    tree.releaseMemory();

    // Busquedas por rango leyendo desde disco.
    FileStore<typename Tree::Node> fstore;
    if (!fstore.open(treePath)) throw std::runtime_error("No se pudo abrir " + treePath);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int32_t> dist(KEY_MIN, KEY_MAX);

    std::vector<KV> found;
    double totalMs = 0;
    uint64_t totalIos = 0;
    uint64_t totalResults = 0;

    for (int q = 0; q < queries; q++) {
        const int32_t lo = dist(rng);
        const int32_t hi = lo + RANGE_SPAN;

        found.clear();
        const uint64_t r0 = fstore.io.reads;
        const double s0 = nowMs();
        Tree::searchRange(fstore, 0, lo, hi, found);
        const double s1 = nowMs();

        totalMs += s1 - s0;
        totalIos += fstore.io.reads - r0;
        totalResults += found.size();
    }

    m.searchMs = totalMs / queries;
    m.searchIos = (double)totalIos / queries;
    m.results = (double)totalResults / queries;

    // Busqueda fija (misma ventana de tiempo) para comparar dos valores de N.
    if (!dumpPath.empty()) {
        found.clear();
        Tree::searchRange(fstore, 0, FIXED_LO, FIXED_LO + RANGE_SPAN, found);
        std::sort(found.begin(), found.end(),
                  [](const KV& a, const KV& b) { return a.key < b.key; });
        std::ofstream out(dumpPath);
        out << "timestamp,temperatura\n";
        for (const KV& kv : found) out << kv.key << ',' << kv.value << '\n';
    }

    fstore.close();
    return m;
}

/** @brief Imprime una fila de resultados por consola. */
void printMetrics(const Metrics& m, double alpha) {
    std::printf("%-12s N=2^%2d  alpha=%.2f  creacion=%9.2f ms  IOs_ins=%12llu  "
                "nodos=%9zu  bytes=%12llu  busq=%8.4f ms  IOs_busq=%9.2f  res=%.0f\n",
                m.name.c_str(), (int)std::llround(std::log2((double)m.n)), alpha, m.buildMs,
                (unsigned long long)m.insertIos, m.nodes, (unsigned long long)m.fileBytes,
                m.searchMs, m.searchIos, m.results);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Autotest
// ---------------------------------------------------------------------------

/** @brief Busqueda por fuerza bruta en un arreglo ordenado por llave. */
std::vector<KV> bruteRange(const std::vector<KV>& sorted, int32_t lo, int32_t hi) {
    auto cmpLo = [](const KV& a, int32_t k) { return a.key < k; };
    auto cmpHi = [](int32_t k, const KV& a) { return k < a.key; };
    auto a = std::lower_bound(sorted.begin(), sorted.end(), lo, cmpLo);
    auto b = std::upper_bound(a, sorted.end(), hi, cmpHi);
    return std::vector<KV>(a, b);
}

/** @brief Compara dos conjuntos de pares por (llave, valor) tras ordenarlos. */
bool samePairs(std::vector<KV> a, std::vector<KV> b) {
    auto cmp = [](const KV& x, const KV& y) {
        return x.key != y.key ? x.key < y.key : x.value < y.value;
    };
    std::sort(a.begin(), a.end(), cmp);
    std::sort(b.begin(), b.end(), cmp);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].key != b[i].key || a[i].value != b[i].value) return false;
    return true;
}

/**
 * @brief Ejecuta pruebas de insercion, invariantes y busqueda para ambos arboles.
 * @return 0 si todas las pruebas pasan; 1 en caso contrario.
 */
int selftest() {
    const int N = 120000;
    std::mt19937_64 rng(2026);

    std::vector<int32_t> keys(N);
    for (int i = 0; i < N; i++) keys[i] = KEY_MIN + i * 7;
    std::shuffle(keys.begin(), keys.end(), rng);

    std::vector<KV> data(N);
    for (int i = 0; i < N; i++) data[i] = KV{keys[i], (float)(keys[i] % 1000) / 10.0f};

    std::vector<KV> sorted = data;
    std::sort(sorted.begin(), sorted.end(),
              [](const KV& a, const KV& b) { return a.key < b.key; });

    bool ok = true;
    auto check = [&](const std::string& what, const std::string& err) {
        if (err.empty()) {
            std::printf("[OK]   %s\n", what.c_str());
        } else {
            std::printf("[FAIL] %s: %s\n", what.c_str(), err.c_str());
            ok = false;
        }
        std::fflush(stdout);
    };

    // --- Arbol B ---
    {
        BTree t;
        t.reserve(N / 150 + 1024);
        for (const KV& kv : data) t.insert(kv);
        check("B: invariantes e insercion", t.validate(sorted));

        const std::string path = "selftest_b.bin";
        t.saveTo(path);
        t.releaseMemory();

        FileStore<BNode> fstore;
        if (!fstore.open(path)) {
            check("B: abrir archivo", "no se pudo abrir");
        } else {
            bool rangesOk = true;
            std::uniform_int_distribution<int32_t> dist(KEY_MIN - 1000, KEY_MIN + N * 7 + 1000);
            for (int q = 0; q < 200 && rangesOk; q++) {
                const int32_t lo = dist(rng);
                const int32_t hi = lo + dist(rng) % 50000;
                std::vector<KV> got;
                BTree::searchRange(fstore, 0, lo, hi, got);
                if (!samePairs(got, bruteRange(sorted, lo, hi))) rangesOk = false;
            }
            check("B: busquedas por rango", rangesOk ? "" : "resultado incorrecto");
            fstore.close();
        }
        fs::remove(path);
    }

    // --- Arbol B-epsilon (ambas configuraciones) ---
    {
        EpsTree<EPS_C_025, EPS_L_025> t(0.5, N / 200 + 1024);
        for (const KV& kv : data) t.insert(kv);
        check("Be(0.25): invariantes e insercion", t.validate(sorted));

        const std::string path = "selftest_e1.bin";
        t.saveTo(path);
        t.releaseMemory();

        FileStore<EpsNode<EPS_C_025, EPS_L_025>> fstore;
        if (!fstore.open(path)) {
            check("Be(0.25): abrir archivo", "no se pudo abrir");
        } else {
            bool rangesOk = true;
            std::uniform_int_distribution<int32_t> dist(KEY_MIN - 1000, KEY_MIN + N * 7 + 1000);
            for (int q = 0; q < 200 && rangesOk; q++) {
                const int32_t lo = dist(rng);
                const int32_t hi = lo + dist(rng) % 50000;
                std::vector<KV> got;
                EpsTree<EPS_C_025, EPS_L_025>::searchRange(fstore, 0, lo, hi, got);
                if (!samePairs(got, bruteRange(sorted, lo, hi))) rangesOk = false;
            }
            check("Be(0.25): busquedas por rango", rangesOk ? "" : "resultado incorrecto");
            fstore.close();
        }
        fs::remove(path);
    }

    {
        EpsTree<EPS_C_075, EPS_L_075> t(0.5, N / 200 + 1024);
        for (const KV& kv : data) t.insert(kv);
        check("Be(0.75): invariantes e insercion", t.validate(sorted));

        const std::string path = "selftest_e2.bin";
        t.saveTo(path);
        t.releaseMemory();

        FileStore<EpsNode<EPS_C_075, EPS_L_075>> fstore;
        if (!fstore.open(path)) {
            check("Be(0.75): abrir archivo", "no se pudo abrir");
        } else {
            bool rangesOk = true;
            std::uniform_int_distribution<int32_t> dist(KEY_MIN - 1000, KEY_MIN + N * 7 + 1000);
            for (int q = 0; q < 200 && rangesOk; q++) {
                const int32_t lo = dist(rng);
                const int32_t hi = lo + dist(rng) % 50000;
                std::vector<KV> got;
                EpsTree<EPS_C_075, EPS_L_075>::searchRange(fstore, 0, lo, hi, got);
                if (!samePairs(got, bruteRange(sorted, lo, hi))) rangesOk = false;
            }
            check("Be(0.75): busquedas por rango", rangesOk ? "" : "resultado incorrecto");
            fstore.close();
        }
        fs::remove(path);
    }

    std::printf(ok ? "\nTodas las pruebas pasaron.\n" : "\nHubo pruebas fallidas.\n");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Experimentos
// ---------------------------------------------------------------------------

/**
 * @brief Ejecuta el experimento principal para N = 2^15 ... 2^26.
 * @param dataPath Ruta de datos.bin.
 * @param outDir   Directorio de salida.
 * @param alpha    Valor de alpha para los arboles B-epsilon.
 * @param queries  Cantidad de busquedas aleatorias por arbol.
 * @param keep     Si es true, conserva los archivos .bin de los arboles.
 * @return 0 si termina correctamente.
 */
int experiment(const std::string& dataPath, const std::string& outDir, double alpha, int queries,
               bool keep, int emin, int emax) {
    fs::create_directories(outDir);
    const std::string csv = outDir + "/resultados.csv";
    if (fs::exists(csv)) fs::remove(csv);

    for (int e = emin; e <= emax; e++) {
        const size_t n = (size_t)1 << e;
        std::printf("\n=== N = 2^%d = %zu pares ===\n", e, n);
        std::fflush(stdout);

        const std::vector<KV> data = readData(dataPath, n);
        const std::string treePath = outDir + "/tree_tmp.bin";

        {
            BTree tree;
            tree.reserve(n / 150 + 1024);
            const std::string dump = (e == 20 || e == 26) ? outDir + "/rango_B_N2e" + std::to_string(e) + ".csv" : "";
            Metrics m = runTree("B", tree, data, treePath, queries, 12345 + e, dump);
            printMetrics(m, alpha);
            appendCsv(csv, m, alpha);
        }
        {
            EpsTree<EPS_C_025, EPS_L_025> tree(alpha, n / 200 + 1024);
            const std::string dump = (e == 20 || e == 26) ? outDir + "/rango_Be025_N2e" + std::to_string(e) + ".csv" : "";
            Metrics m = runTree("Be_c4_l506", tree, data, treePath, queries, 12345 + e, dump);
            printMetrics(m, alpha);
            appendCsv(csv, m, alpha);
        }
        {
            EpsTree<EPS_C_075, EPS_L_075> tree(alpha, n / 200 + 1024);
            const std::string dump = (e == 20 || e == 26) ? outDir + "/rango_Be075_N2e" + std::to_string(e) + ".csv" : "";
            Metrics m = runTree("Be_c126_l384", tree, data, treePath, queries, 12345 + e, dump);
            printMetrics(m, alpha);
            appendCsv(csv, m, alpha);
        }

        if (!keep) fs::remove(treePath);
    }

    std::printf("\nResultados escritos en %s\n", csv.c_str());
    return 0;
}

/**
 * @brief Barrido de alpha para un N fijo (bonus).
 * @param dataPath Ruta de datos.bin.
 * @param outDir   Directorio de salida.
 * @param n        Cantidad de pares a insertar.
 * @param queries  Cantidad de busquedas por configuracion.
 * @return 0 si termina correctamente.
 */
int alphaSweep(const std::string& dataPath, const std::string& outDir, size_t n, int queries) {
    fs::create_directories(outDir);
    const std::string csv = outDir + "/alpha.csv";
    if (fs::exists(csv)) fs::remove(csv);

    const std::vector<KV> data = readData(dataPath, n);
    const std::string treePath = outDir + "/tree_tmp.bin";

    const double alphas[] = {0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80};
    for (double a : alphas) {
        std::printf("\n=== alpha = %.2f (N = %zu) ===\n", a, n);
        std::fflush(stdout);
        EpsTree<EPS_C_025, EPS_L_025> tree(a, n / 200 + 1024);
        Metrics m = runTree("Be_c4_l506", tree, data, treePath, queries, 999, "");
        printMetrics(m, a);
        appendCsv(csv, m, a);
    }
    fs::remove(treePath);

    std::printf("\nResultados escritos en %s\n", csv.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

/** @brief Punto de entrada: interpreta los argumentos y despacha el modo de ejecucion. */
int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::printf("Uso:\n"
                        "  %s selftest\n"
                        "  %s experiment --data datos.bin --out resultados [--alpha 0.5] [--queries 50]\n"
                        "                [--emin 15] [--emax 26] [--keep]\n"
                        "  %s alpha --data datos.bin --out resultados [--n 24] [--queries 20]\n",
                        argv[0], argv[0], argv[0]);
            return 1;
        }

        const std::string mode = argv[1];
        if (mode == "selftest") return selftest();

        std::string dataPath = "datos.bin";
        std::string outDir = "resultados";
        double alpha = 0.5;
        int queries = 50;
        int expN = 24;
        int emin = 15, emax = 26;
        bool keep = false;

        for (int i = 2; i < argc; i++) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("Falta valor para " + arg);
                return argv[++i];
            };
            if (arg == "--data") dataPath = next();
            else if (arg == "--out") outDir = next();
            else if (arg == "--alpha") alpha = std::stod(next());
            else if (arg == "--queries") queries = std::stoi(next());
            else if (arg == "--n") expN = std::stoi(next());
            else if (arg == "--emin") emin = std::stoi(next());
            else if (arg == "--emax") emax = std::stoi(next());
            else if (arg == "--keep") keep = true;
            else throw std::runtime_error("Argumento desconocido: " + arg);
        }

        if (mode == "experiment") return experiment(dataPath, outDir, alpha, queries, keep, emin, emax);
        if (mode == "alpha") return alphaSweep(dataPath, outDir, (size_t)1 << expN, queries);

        throw std::runtime_error("Modo desconocido: " + mode);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Error: %s\n", ex.what());
        return 1;
    }
}
